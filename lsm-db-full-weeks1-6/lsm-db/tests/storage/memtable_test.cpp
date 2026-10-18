// MemTable behaviour and concurrency. These tests validate the locking design:
// no lost writes, readers never see torn state, and Freeze() is a clean cut-off.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "lsmdb/storage/memtable.h"

using namespace lsmdb::storage;
using State = MemTable::LookupState;

TEST(MemTable, PutThenGet) {
  MemTable t;
  ASSERT_TRUE(t.Put("k", "v", 1));
  auto r = t.Get("k");
  EXPECT_EQ(r.state, State::kFound);
  EXPECT_EQ(r.value, "v");
  EXPECT_EQ(r.seq, 1u);
}

TEST(MemTable, MissingKeyIsNotFound) {
  MemTable t;
  ASSERT_TRUE(t.Put("a", "1", 1));
  EXPECT_EQ(t.Get("b").state, State::kNotFound);
  EXPECT_EQ(t.Get("").state, State::kNotFound);
}

TEST(MemTable, NewestVersionWinsAndSnapshotsSeeOlderVersions) {
  MemTable t;
  ASSERT_TRUE(t.Put("k", "v1", 10));
  ASSERT_TRUE(t.Put("k", "v2", 20));
  ASSERT_TRUE(t.Put("k", "v3", 30));
  EXPECT_EQ(t.Get("k").value, "v3");
  EXPECT_EQ(t.Get("k", 29).value, "v2");
  EXPECT_EQ(t.Get("k", 20).value, "v2");   // boundary: seq <= snapshot
  EXPECT_EQ(t.Get("k", 10).value, "v1");
  EXPECT_EQ(t.Get("k", 9).state, State::kNotFound);  // before the first version
}

TEST(MemTable, TombstoneHidesOlderValueButNotFromOlderSnapshot) {
  MemTable t;
  ASSERT_TRUE(t.Put("k", "v", 5));
  ASSERT_TRUE(t.Delete("k", 8));
  auto now = t.Get("k");
  EXPECT_EQ(now.state, State::kDeleted);
  EXPECT_EQ(now.seq, 8u);
  EXPECT_EQ(t.Get("k", 7).state, State::kFound);
  ASSERT_TRUE(t.Put("k", "again", 9));
  EXPECT_EQ(t.Get("k").value, "again");
}

TEST(MemTable, KeysDoNotLeakIntoNeighbours) {
  MemTable t;
  ASSERT_TRUE(t.Put("ab", "1", 1));
  ASSERT_TRUE(t.Put("abc", "2", 1));
  EXPECT_EQ(t.Get("ab").value, "1");
  EXPECT_EQ(t.Get("abc").value, "2");
  EXPECT_EQ(t.Get("a").state, State::kNotFound);
}

TEST(MemTable, SameKeyAndSeqOverwritesWhichMakesReplayIdempotent) {
  MemTable t;
  ASSERT_TRUE(t.Put("k", "first", 7));
  ASSERT_TRUE(t.Put("k", "second", 7));
  EXPECT_EQ(t.EntryCount(), 1u);
  EXPECT_EQ(t.Get("k").value, "second");
}

TEST(MemTable, EntriesAreSortedKeyAscSeqDesc) {
  MemTable t;
  ASSERT_TRUE(t.Put("b", "1", 3));
  ASSERT_TRUE(t.Put("a", "1", 1));
  ASSERT_TRUE(t.Put("b", "2", 9));
  ASSERT_TRUE(t.Delete("a", 5));
  auto e = t.Entries();
  ASSERT_EQ(e.size(), 4u);
  EXPECT_EQ(e[0].key, "a"); EXPECT_EQ(e[0].seq, 5u); EXPECT_EQ(e[0].type, EntryType::kDelete);
  EXPECT_EQ(e[1].key, "a"); EXPECT_EQ(e[1].seq, 1u);
  EXPECT_EQ(e[2].key, "b"); EXPECT_EQ(e[2].seq, 9u);
  EXPECT_EQ(e[3].key, "b"); EXPECT_EQ(e[3].seq, 3u);
}

TEST(MemTable, FreezeRejectsWritesButKeepsReads) {
  MemTable t;
  ASSERT_TRUE(t.Put("k", "v", 1));
  EXPECT_FALSE(t.frozen());
  t.Freeze();
  EXPECT_TRUE(t.frozen());
  EXPECT_FALSE(t.Put("k2", "v", 2));
  EXPECT_FALSE(t.Delete("k", 3));
  EXPECT_EQ(t.Get("k").value, "v");
  EXPECT_EQ(t.Get("k2").state, State::kNotFound);
  EXPECT_EQ(t.EntryCount(), 1u);
}

TEST(MemTable, ApproximateBytesGrowsWithData) {
  MemTable t;
  EXPECT_EQ(t.ApproximateBytes(), 0u);
  ASSERT_TRUE(t.Put("key", std::string(1000, 'x'), 1));
  EXPECT_GE(t.ApproximateBytes(), 1003u);
  std::size_t before = t.ApproximateBytes();
  ASSERT_TRUE(t.Put("key", std::string(10, 'x'), 1));  // overwrite with smaller value
  EXPECT_LT(t.ApproximateBytes(), before);
}

TEST(MemTableConcurrency, ParallelWritersLoseNothing) {
  MemTable t;
  constexpr int kThreads = 8, kPer = 2000;
  std::atomic<std::uint64_t> seq{1};
  std::vector<std::thread> threads;
  for (int w = 0; w < kThreads; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPer; ++i) {
        ASSERT_TRUE(t.Put("w" + std::to_string(w) + "-" + std::to_string(i), "v", seq++));
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(t.EntryCount(), static_cast<std::size_t>(kThreads * kPer));
  for (int w = 0; w < kThreads; ++w) {
    for (int i = 0; i < kPer; i += 97) {
      EXPECT_EQ(t.Get("w" + std::to_string(w) + "-" + std::to_string(i)).state, State::kFound);
    }
  }
}

TEST(MemTableConcurrency, ReadersSeeMonotonicConsistentVersionsWhileWriterRuns) {
  MemTable t;
  constexpr std::uint64_t kWrites = 5000;
  std::atomic<bool> done{false};
  std::atomic<int> violations{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      std::uint64_t last = 0;
      while (!done) {
        std::this_thread::yield();  // lets the writer run even on a single-core machine
        auto res = t.Get("counter");
        if (res.state != State::kFound) continue;
        // value is always the decimal of its own seq: a torn read would break this
        if (res.value != std::to_string(res.seq)) ++violations;
        if (res.seq < last) ++violations;  // versions never go backwards
        last = res.seq;
      }
    });
  }
  for (std::uint64_t i = 1; i <= kWrites; ++i) ASSERT_TRUE(t.Put("counter", std::to_string(i), i));
  done = true;
  for (auto& th : readers) th.join();
  EXPECT_EQ(violations, 0);
  EXPECT_EQ(t.Get("counter").seq, kWrites);
}

TEST(MemTableConcurrency, FreezeIsACleanCutOffWhileWritersRun) {
  MemTable t;
  std::atomic<std::uint64_t> seq{1};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; !stop; ++i) {
        if (t.Put("w" + std::to_string(w) + "-" + std::to_string(i), "v", seq++)) ++accepted;
        else break;
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  t.Freeze();
  const std::size_t at_freeze = t.EntryCount();
  stop = true;
  for (auto& th : writers) th.join();
  // nothing slipped in after Freeze returned, and every accepted write is present
  EXPECT_EQ(t.EntryCount(), at_freeze);
  EXPECT_EQ(accepted.load(), at_freeze);
}

TEST(MemTableConcurrency, ExclusiveOnlyModeIsStillCorrect) {
  MemTable t(MemTable::LockMode::kExclusiveOnly);
  ASSERT_TRUE(t.Put("k", "v", 1));
  std::vector<std::thread> threads;
  std::atomic<int> bad{0};
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < 1000; ++j) if (t.Get("k").value != "v") ++bad;
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(bad, 0);
}
