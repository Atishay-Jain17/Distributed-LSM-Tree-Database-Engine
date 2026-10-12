// End-to-end single-node flow (Week 4 Thursday review): WAL -> MemTable -> flush -> tables ->
// compaction, then a "crash" and WAL recovery into the same store. Uses the stand-in WAL and the
// reference table store; swap in the real ones when they land and keep these checks.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <random>

#include <unistd.h>

#include "lsmdb/lifecycle/lsm_store.h"
#include "lsmdb/lifecycle/reference_table_store.h"
#include "lsmdb/recovery/recovery.h"
#include "lsmdb/recovery/reference_wal.h"

using namespace lsmdb;
using namespace lsmdb::lifecycle;
namespace fs = std::filesystem;

namespace {

class FlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("lsmdb_week4_flow_" + std::to_string(::getpid()) + "_" +
                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_ / "tables");
    wal_path_ = (dir_ / "wal.log").string();
  }
  void TearDown() override { std::error_code ec; fs::remove_all(dir_, ec); }
  fs::path dir_;
  std::string wal_path_;
};

}  // namespace

TEST_F(FlowTest, CrashAfterFlushesAndCompactionsRecoversEverything) {
  std::map<std::string, std::string> model;
  SequenceNumber flushed = 0;
  std::mutex mu;
  auto tables = std::make_shared<ReferenceTableStore>(dir_ / "tables",
                                                      ReferenceTableStoreOptions{.compaction_trigger = 3});
  {
    LsmOptions opt;
    opt.memtable_bytes = 600;
    opt.on_flushed = [&](SequenceNumber s) { std::lock_guard<std::mutex> l(mu); flushed = std::max(flushed, s); };
    LsmStore db(tables, opt);
    recovery::ReferenceWalWriter wal(wal_path_);

    std::mt19937 rng(7);
    for (int i = 0; i < 600; ++i) {
      std::string k = "k" + std::to_string(rng() % 80);
      if (rng() % 4 == 0) {
        db.Delete(k);
        wal.Append({db.LastSequence(), api::ValueType::kTombstone, k, ""});
        model.erase(k);
      } else {
        std::string v = "v" + std::to_string(i);
        db.Put(k, v);
        wal.Append({db.LastSequence(), api::ValueType::kValue, k, v});
        model[k] = v;
      }
      if (i == 300) db.WaitForIdle();   // let some flushes + a compaction finish mid-run
    }
    // Scope end == crash: the destructor does NOT flush the memtable; the WAL has the rest.
  }
  ASSERT_GT(flushed, 0u) << "test should have flushed something";
  ASSERT_GE(tables->TableCount(), 1u);

  // Restart: same tables (the manifest's job in the real system), start after what they hold,
  // replay the WAL for everything newer.
  LsmOptions opt;
  opt.memtable_bytes = 600;
  opt.start_sequence = flushed;
  LsmStore db(tables, opt);
  recovery::ReferenceWalReader reader(wal_path_);
  recovery::RecoveryStats st = recovery::Recover(reader, db, flushed);
  EXPECT_GT(st.records_applied, 0u);
  EXPECT_FALSE(st.dropped_torn_tail);

  for (int i = 0; i < 80; ++i) {
    std::string k = "k" + std::to_string(i);
    auto it = model.find(k);
    auto got = db.Get(k);
    if (it == model.end()) EXPECT_FALSE(got.has_value()) << k;
    else { ASSERT_TRUE(got.has_value()) << k; EXPECT_EQ(*got, it->second) << k; }
  }

  // The recovered store keeps working: new writes get new sequences and can flush.
  db.Put("after-restart", "yes");
  db.Flush();
  EXPECT_EQ(db.Get("after-restart"), std::optional<std::string>("yes"));
}

TEST_F(FlowTest, RecoveryAlsoWorksWithEmptyTables) {
  // Nothing was ever flushed (or the table files were lost): replaying the whole WAL is enough.
  {
    LsmStore db(std::make_shared<ReferenceTableStore>(dir_ / "tables"));
    recovery::ReferenceWalWriter wal(wal_path_);
    for (int i = 0; i < 20; ++i) {
      db.Put("k" + std::to_string(i), "v");
      wal.Append({db.LastSequence(), api::ValueType::kValue, "k" + std::to_string(i), "v"});
    }
  }
  fs::remove_all(dir_ / "tables");
  fs::create_directories(dir_ / "tables");
  LsmStore db(std::make_shared<ReferenceTableStore>(dir_ / "tables"));
  recovery::ReferenceWalReader reader(wal_path_);
  recovery::Recover(reader, db, 0);
  for (int i = 0; i < 20; ++i) EXPECT_EQ(db.Get("k" + std::to_string(i)), std::optional<std::string>("v"));
}
