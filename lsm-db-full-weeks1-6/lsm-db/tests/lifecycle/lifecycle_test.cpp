// Flush lifecycle tests (Week 4 Suhani): MemTable rotation, flush worker, compaction scheduling,
// reads while flushing/compacting, and SSTable file lifecycle.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <set>
#include <thread>
#include <vector>

#include <unistd.h>

#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/lifecycle/lsm_store.h"
#include "lsmdb/lifecycle/reference_table_store.h"

using namespace lsmdb::lifecycle;
using lsmdb::api::LookupState;
using lsmdb::api::ValueType;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const std::string& name) {
  fs::path p = fs::temp_directory_path() / ("lsmdb_week4_" + name + "_" + std::to_string(::getpid()));
  fs::remove_all(p);
  return p;
}

// Wraps a TableStore so a test can hold a flush (or compaction) open, or make flushes fail.
class ControlledStore : public TableStore {
 public:
  explicit ControlledStore(std::shared_ptr<ReferenceTableStore> inner) : inner_(std::move(inner)) {}
  void AddTable(const MemTable& imm) override {
    std::unique_lock<std::mutex> l(mu_);
    ++flush_started_;
    cv_.notify_all();
    cv_.wait(l, [&] { return !hold_flush_; });
    if (fail_flush_) throw std::runtime_error("disk full");
    l.unlock();
    inner_->AddTable(imm);
  }
  LookupResult Get(const std::string& k, SequenceNumber s) const override { return inner_->Get(k, s); }
  bool NeedsCompaction() const override { return inner_->NeedsCompaction(); }
  void Compact(SequenceNumber oldest) override {
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait(l, [&] { return !hold_compaction_; });
    }
    inner_->Compact(oldest);
  }
  void HoldFlush(bool v) { std::lock_guard<std::mutex> l(mu_); hold_flush_ = v; cv_.notify_all(); }
  void HoldCompaction(bool v) { std::lock_guard<std::mutex> l(mu_); hold_compaction_ = v; cv_.notify_all(); }
  void FailFlush(bool v) { std::lock_guard<std::mutex> l(mu_); fail_flush_ = v; }
  void WaitFlushStarted(int n) {
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait(l, [&] { return flush_started_ >= n; });
  }
  ReferenceTableStore& inner() { return *inner_; }

 private:
  std::shared_ptr<ReferenceTableStore> inner_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool hold_flush_ = false, hold_compaction_ = false, fail_flush_ = false;
  int flush_started_ = 0;
};

std::string Key(int i) {
  char b[16];
  std::snprintf(b, sizeof b, "key%06d", i);
  return b;
}

std::set<fs::path> FilesIn(const fs::path& d) {
  std::set<fs::path> s;
  for (const auto& e : fs::directory_iterator(d)) s.insert(e.path());
  return s;
}

class LifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = TempDir(::testing::UnitTest::GetInstance()->current_test_info()->name()); }
  void TearDown() override { fs::remove_all(dir_); }
  fs::path dir_;
};

LsmOptions Small(std::size_t bytes = 512) {
  LsmOptions o;
  o.memtable_bytes = bytes;
  return o;
}

}  // namespace

// ---- rotation and flush -----------------------------------------------------------------

TEST_F(LifecycleTest, RotatesWhenThresholdReachedAndFlushesToTable) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{1000});
  LsmStore db(ts, Small(512));
  for (int i = 0; i < 200; ++i) db.Put(Key(i), std::string(20, 'x'));
  db.Flush();
  auto st = db.stats();
  EXPECT_GT(st.rotations, 3u);
  EXPECT_EQ(st.flushes, st.rotations);
  EXPECT_EQ(ts->TableCount(), st.flushes);
  EXPECT_EQ(db.FlushedUpTo(), db.LastSequence());
  for (int i = 0; i < 200; ++i) ASSERT_EQ(db.Get(Key(i)), std::optional<std::string>(std::string(20, 'x')));
}

TEST_F(LifecycleTest, NoRotationBelowThreshold) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_);
  LsmStore db(ts, Small(1 << 20));
  for (int i = 0; i < 50; ++i) db.Put(Key(i), "v");
  EXPECT_EQ(db.stats().rotations, 0u);
  EXPECT_EQ(ts->TableCount(), 0u);
  EXPECT_EQ(db.FlushedUpTo(), 0u);
}

TEST_F(LifecycleTest, FlushOnEmptyStoreIsNoOp) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_);
  LsmStore db(ts, Small());
  EXPECT_NO_THROW(db.Flush());
  EXPECT_EQ(ts->TableCount(), 0u);
}

TEST_F(LifecycleTest, OnFlushedReportsSequencesInIncreasingOrder) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{1000});
  std::mutex m;
  std::vector<SequenceNumber> seen;
  LsmOptions o = Small(256);
  o.on_flushed = [&](SequenceNumber s) { std::lock_guard<std::mutex> l(m); seen.push_back(s); };
  LsmStore db(ts, o);
  for (int i = 0; i < 100; ++i) db.Put(Key(i), "value-value");
  db.Flush();
  std::lock_guard<std::mutex> l(m);
  ASSERT_FALSE(seen.empty());
  EXPECT_TRUE(std::is_sorted(seen.begin(), seen.end()));
  EXPECT_EQ(seen.back(), 100u);
}

// ---- reads and writes during a flush ---------------------------------------------------

TEST_F(LifecycleTest, ReadsSeeImmutableMemTableWhileFlushIsRunning) {
  auto ctl = std::make_shared<ControlledStore>(std::make_shared<ReferenceTableStore>(dir_));
  LsmStore db(ctl, Small(256));
  ctl->HoldFlush(true);
  int i = 0;
  while (db.stats().rotations == 0) db.Put(Key(i++), "old");   // fills the first MemTable
  ctl->WaitFlushStarted(1);                                      // flush is now stuck mid-way
  db.Put("after-rotation", "new");                              // goes to the new active MemTable
  for (int j = 0; j < i; ++j) ASSERT_EQ(db.Get(Key(j)), std::optional<std::string>("old")) << j;
  EXPECT_EQ(db.Get("after-rotation"), std::optional<std::string>("new"));
  EXPECT_EQ(db.FlushedUpTo(), 0u);
  ctl->HoldFlush(false);
  db.Flush();
  for (int j = 0; j < i; ++j) ASSERT_EQ(db.Get(Key(j)), std::optional<std::string>("old")) << j;
}

TEST_F(LifecycleTest, DeleteInNewerLayerHidesValueInOlderLayer) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{1000});
  LsmStore db(ts, Small(1 << 20));
  db.Put("k", "v");
  db.Flush();                         // value now in a table
  db.Delete("k");                     // tombstone in the MemTable
  EXPECT_FALSE(db.Get("k").has_value());
  db.Flush();                         // tombstone in a newer table
  EXPECT_FALSE(db.Get("k").has_value());
  db.Put("k", "v2");
  EXPECT_EQ(db.Get("k"), std::optional<std::string>("v2"));
}

TEST_F(LifecycleTest, WriterStallsWhileFlushIsBlockedThenContinues) {
  auto ctl = std::make_shared<ControlledStore>(std::make_shared<ReferenceTableStore>(dir_));
  LsmStore db(ctl, Small(200));
  ctl->HoldFlush(true);
  std::atomic<int> written{0};
  std::thread writer([&] {
    for (int i = 0; i < 100; ++i) { db.Put(Key(i), std::string(20, 'v')); written = i + 1; }
  });
  ctl->WaitFlushStarted(1);
  // Wait until the writer is blocked: no progress for a while and a stall was recorded.
  for (int spin = 0; spin < 200 && db.stats().write_stalls == 0; ++spin)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_GE(db.stats().write_stalls, 1u);
  const int stuck_at = written.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(written.load(), stuck_at);          // bounded memory: writer is waiting, not growing
  EXPECT_LT(stuck_at, 100);
  ctl->HoldFlush(false);
  writer.join();
  EXPECT_EQ(written.load(), 100);
  db.Flush();
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(db.Get(Key(i)).has_value());
}

TEST_F(LifecycleTest, FlushFailureKeepsDataReadableAndSurfacesToWriters) {
  auto ctl = std::make_shared<ControlledStore>(std::make_shared<ReferenceTableStore>(dir_));
  LsmStore db(ctl, Small(200));
  ctl->FailFlush(true);
  int i = 0;
  EXPECT_THROW({ for (; i < 1000; ++i) db.Put(Key(i), std::string(20, 'v')); }, std::runtime_error);
  EXPECT_EQ(db.background_error(), "disk full");
  EXPECT_THROW(db.Flush(), std::runtime_error);
  for (int j = 0; j < i; ++j) ASSERT_TRUE(db.Get(Key(j)).has_value()) << j;   // nothing lost
  EXPECT_EQ(db.FlushedUpTo(), 0u);                                            // WAL must be kept
}

TEST_F(LifecycleTest, ConcurrentWritersAndReadersDuringFlushes) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{1000});
  LsmStore db(ts, Small(2048));
  constexpr int kWriters = 3, kPerWriter = 400;
  std::vector<std::atomic<int>> confirmed(kWriters);
  for (auto& c : confirmed) c = -1;
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};

  std::vector<std::thread> ts_;
  for (int w = 0; w < kWriters; ++w) {
    ts_.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        db.Put("w" + std::to_string(w) + "-" + std::to_string(i), "v" + std::to_string(i));
        confirmed[w] = i;
      }
    });
  }
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      while (!stop) {
        for (int w = 0; w < kWriters; ++w) {
          int c = confirmed[w].load();
          if (c < 0) continue;
          for (int i : {0, c / 2, c}) {         // anything already acknowledged must be visible
            auto v = db.Get("w" + std::to_string(w) + "-" + std::to_string(i));
            if (!v || *v != "v" + std::to_string(i)) ++bad;
          }
        }
      }
    });
  }
  for (auto& t : ts_) t.join();
  stop = true;
  for (auto& t : readers) t.join();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_GT(db.stats().rotations, 5u);
  db.Flush();
  for (int w = 0; w < kWriters; ++w)
    for (int i = 0; i < kPerWriter; ++i)
      ASSERT_EQ(db.Get("w" + std::to_string(w) + "-" + std::to_string(i)),
                std::optional<std::string>("v" + std::to_string(i)));
}

// ---- compaction scheduling and reads during compaction ----------------------------------

TEST_F(LifecycleTest, CompactionIsScheduledAfterFlushesAndShrinksTableCount) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{3});
  LsmStore db(ts, Small(256));
  for (int i = 0; i < 300; ++i) db.Put(Key(i), std::string(16, 'a'));
  db.Flush();
  db.WaitForIdle();
  EXPECT_GE(db.stats().compactions, 1u);
  EXPECT_LT(ts->TableCount(), 3u);
  for (int i = 0; i < 300; ++i) ASSERT_EQ(db.Get(Key(i)), std::optional<std::string>(std::string(16, 'a')));
}

TEST_F(LifecycleTest, ReadsStayCorrectWhileCompactionRuns) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{3});
  LsmStore db(ts, Small(512));
  constexpr int kKeys = 40;
  std::vector<std::atomic<int>> version(kKeys);
  for (auto& v : version) v = 0;
  for (int k = 0; k < kKeys; ++k) db.Put(Key(k), "0");
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};

  std::thread writer([&] {
    for (int round = 1; round <= 60; ++round) {
      for (int k = 0; k < kKeys; ++k) {
        db.Put(Key(k), std::to_string(round));
        version[k] = round;                    // acknowledged
      }
    }
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      int last_seen[kKeys] = {0};
      while (!stop) {
        for (int k = 0; k < kKeys; ++k) {
          const int floor = version[k].load();
          auto v = db.Get(Key(k));
          if (!v) { ++bad; continue; }                       // key must never disappear
          const int got = std::stoi(*v);
          if (got < floor) ++bad;                            // acknowledged write lost
          if (got < last_seen[k]) ++bad;                     // went backwards
          last_seen[k] = got;
        }
      }
    });
  }
  writer.join();
  stop = true;
  for (auto& t : readers) t.join();
  EXPECT_EQ(bad.load(), 0);
  db.Flush();
  db.WaitForIdle();
  EXPECT_GE(db.stats().compactions, 1u);
  for (int k = 0; k < kKeys; ++k) ASSERT_EQ(db.Get(Key(k)), std::optional<std::string>("60")) << k;
}

TEST_F(LifecycleTest, SnapshotKeepsOldVersionsAliveAcrossCompaction) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{2});
  LsmStore db(ts, Small(1 << 20));
  db.Put("a", "1");
  db.Put("b", "1");
  db.Flush();
  auto snap = db.AcquireSnapshot();
  db.Put("a", "2");
  db.Delete("b");
  db.Flush();                               // 2 tables -> compaction wants to run
  db.Put("a", "3");
  db.Flush();
  db.WaitForIdle();
  EXPECT_GE(db.stats().compactions, 1u);
  // The snapshot still sees the world as it was; the current view sees the new state.
  EXPECT_EQ(db.GetAt("a", snap.sequence()).value, "1");
  EXPECT_EQ(db.GetAt("b", snap.sequence()).state, LookupState::kFound);
  EXPECT_EQ(db.Get("a"), std::optional<std::string>("3"));
  EXPECT_FALSE(db.Get("b").has_value());
  snap.Release();
}

TEST_F(LifecycleTest, CompactionDropsDeletedKeysOnceNoSnapshotNeedsThem) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{2});
  LsmStore db(ts, Small(1 << 20));
  db.Put("gone", "v");
  db.Flush();
  db.Delete("gone");
  db.Flush();
  db.WaitForIdle();
  EXPECT_FALSE(db.Get("gone").has_value());
  EXPECT_EQ(ts->TableCount(), 0u);          // value + tombstone cancelled out, no file left
  EXPECT_TRUE(FilesIn(dir_).empty());
}

TEST_F(LifecycleTest, CompactionFailureStopsSchedulingButWritesContinue) {
  struct Failing : ReferenceTableStore {
    using ReferenceTableStore::ReferenceTableStore;
    void Compact(SequenceNumber) override { throw std::runtime_error("bad block"); }
  };
  auto ts = std::make_shared<Failing>(dir_, ReferenceTableStoreOptions{2});
  LsmStore db(ts, Small(1 << 20));
  for (int r = 0; r < 4; ++r) { db.Put(Key(r), "v"); db.Flush(); }
  db.WaitForIdle();
  EXPECT_EQ(db.stats().compactions, 0u);
  EXPECT_NO_THROW(db.Put("still", "works"));
  EXPECT_EQ(db.Get("still"), std::optional<std::string>("works"));
  EXPECT_EQ(db.Get(Key(0)), std::optional<std::string>("v"));
}

// ---- file lifecycle ---------------------------------------------------------------------

TEST_F(LifecycleTest, FilesAreCreatedReplacedAndCleanedUp) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{3});
  LsmStore db(ts, Small(1 << 20));
  for (int r = 0; r < 2; ++r) { db.Put(Key(r), "v"); db.Flush(); }
  db.WaitForIdle();
  EXPECT_EQ(FilesIn(dir_).size(), 2u);                       // created: one file per flush
  auto before = FilesIn(dir_);

  db.Put(Key(2), "v");
  db.Flush();                                                // third table triggers compaction
  db.WaitForIdle();
  auto after = FilesIn(dir_);
  EXPECT_EQ(after.size(), 1u);                               // replaced by one merged file
  for (const auto& f : before) EXPECT_EQ(after.count(f), 0u);   // old inputs are gone
  for (const auto& f : after) EXPECT_EQ(f.extension(), ".sst"); // no temp files left behind
  const auto live = ts->LivePaths();
  EXPECT_EQ(std::set<fs::path>(live.begin(), live.end()), after);
  for (int r = 0; r < 3; ++r) EXPECT_EQ(db.Get(Key(r)), std::optional<std::string>("v"));
}

TEST_F(LifecycleTest, ObsoleteFileSurvivesUntilLastReaderIsDone) {
  ReferenceTableStore ts(dir_, ReferenceTableStoreOptions{2});
  lsmdb::api::ReferenceMemTable m1, m2;
  m1.Add(1, ValueType::kValue, "a", "1");
  m2.Add(2, ValueType::kValue, "b", "2");
  ts.AddTable(m1);
  ts.AddTable(m2);
  auto pin = ts.Pin();                         // a slow reader holds both tables
  auto old_files = FilesIn(dir_);
  ASSERT_EQ(old_files.size(), 2u);
  ts.Compact(10);
  EXPECT_EQ(ts.TableCount(), 1u);
  for (const auto& f : old_files) EXPECT_TRUE(fs::exists(f)) << f;   // still readable by the pin
  EXPECT_EQ(FilesIn(dir_).size(), 3u);
  pin.reset();                                 // reader finishes
  EXPECT_EQ(FilesIn(dir_).size(), 1u);
  EXPECT_EQ(ts.Get("a", 10).value, "1");
  EXPECT_EQ(ts.Get("b", 10).value, "2");
}

TEST_F(LifecycleTest, FlushedDuringCompactionTableStaysNewer) {
  auto ctl = std::make_shared<ControlledStore>(std::make_shared<ReferenceTableStore>(dir_, ReferenceTableStoreOptions{2}));
  LsmStore db(ctl, Small(1 << 20));
  db.Put("k", "old"); db.Flush();
  ctl->HoldCompaction(true);
  db.Put("x", "1"); db.Flush();                // 2 tables: compaction starts and is held
  for (int i = 0; i < 100 && db.stats().compactions == 0; ++i) {
    db.Put("k", "new"); db.Flush();            // flushes land while the compaction is in flight
    break;
  }
  ctl->HoldCompaction(false);
  db.WaitForIdle();
  EXPECT_EQ(db.Get("k"), std::optional<std::string>("new"));
  EXPECT_EQ(db.Get("x"), std::optional<std::string>("1"));
}

// ---- recovery hand-off ------------------------------------------------------------------

TEST_F(LifecycleTest, RestoreContinuesSequencesAndStartSequenceIsHonoured) {
  auto ts = std::make_shared<ReferenceTableStore>(dir_);
  LsmOptions o = Small(1 << 20);
  o.start_sequence = 100;                      // tables already hold everything up to 100
  LsmStore db(ts, o);
  EXPECT_EQ(db.LastSequence(), 100u);
  EXPECT_EQ(db.FlushedUpTo(), 100u);
  db.Restore(105, ValueType::kValue, "r", "from-wal");
  db.Put("n", "new");
  EXPECT_EQ(db.LastSequence(), 106u);
  EXPECT_EQ(db.Get("r"), std::optional<std::string>("from-wal"));
  EXPECT_THROW(db.Restore(50, ValueType::kValue, "x", "y"), std::logic_error);
  db.Flush();
  EXPECT_EQ(db.FlushedUpTo(), 106u);
}

TEST_F(LifecycleTest, DestructorDoesNotHangWithPendingWork) {
  auto ctl = std::make_shared<ControlledStore>(std::make_shared<ReferenceTableStore>(dir_));
  {
    LsmStore db(ctl, Small(100));
    ctl->HoldFlush(true);
    std::thread t([&] { try { for (int i = 0; i < 50; ++i) db.Put(Key(i), "vvvvvvvvvv"); } catch (...) {} });
    ctl->WaitFlushStarted(1);
    ctl->HoldFlush(false);                     // let the in-flight flush finish
    t.join();
  }                                            // ~LsmStore joins both workers
  SUCCEED();
}
