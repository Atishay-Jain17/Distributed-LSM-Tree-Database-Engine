// API-level tests: GET/PUT/DELETE semantics on top of a MemTable (Week 2 Suhani).
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/api/reference_memtable.h"

using namespace lsmdb::api;

namespace {
class ApiTest : public ::testing::Test {
 protected:
  std::shared_ptr<ReferenceMemTable> mt_ = std::make_shared<ReferenceMemTable>();
  MemTableBackend db_{mt_};
};
}  // namespace

TEST_F(ApiTest, PutThenGet) {
  db_.Put("name", "suhani");
  EXPECT_EQ(db_.Get("name"), std::optional<std::string>("suhani"));
}

TEST_F(ApiTest, MissingKeyReturnsNullopt) { EXPECT_FALSE(db_.Get("missing").has_value()); }

TEST_F(ApiTest, OverwriteReturnsLatestValue) {
  db_.Put("k", "v1");
  db_.Put("k", "v2");
  db_.Put("k", "v3");
  EXPECT_EQ(*db_.Get("k"), "v3");
  EXPECT_EQ(mt_->EntryCount(), 3u);  // versions are kept, not replaced in place
}

TEST_F(ApiTest, EmptyValueIsStoredNotMissing) {
  db_.Put("k", "");
  ASSERT_TRUE(db_.Get("k").has_value());
  EXPECT_EQ(*db_.Get("k"), "");
}

TEST_F(ApiTest, DeleteHidesKey) {
  db_.Put("k", "v");
  db_.Delete("k");
  EXPECT_FALSE(db_.Get("k").has_value());
}

TEST_F(ApiTest, DeleteIsIdempotentAndWorksOnMissingKey) {
  EXPECT_NO_THROW(db_.Delete("never-existed"));
  EXPECT_NO_THROW(db_.Delete("never-existed"));
  EXPECT_FALSE(db_.Get("never-existed").has_value());
}

TEST_F(ApiTest, DeleteWritesATombstoneEvenForMissingKey) {
  db_.Delete("ghost");
  EXPECT_EQ(mt_->EntryCount(), 1u);  // needed to shadow older on-disk versions later
  EXPECT_EQ(db_.GetAt("ghost", db_.LastSequence()).state, LookupState::kDeleted);
}

TEST_F(ApiTest, PutAfterDeleteResurrectsKey) {
  db_.Put("k", "a");
  db_.Delete("k");
  db_.Put("k", "b");
  EXPECT_EQ(*db_.Get("k"), "b");
}

TEST_F(ApiTest, SnapshotReadsSeeOldVersions) {
  db_.Put("k", "v1");
  const auto snap1 = db_.LastSequence();
  db_.Put("k", "v2");
  const auto snap2 = db_.LastSequence();
  db_.Delete("k");
  const auto snap3 = db_.LastSequence();
  EXPECT_EQ(db_.GetAt("k", snap1).value, "v1");
  EXPECT_EQ(db_.GetAt("k", snap2).value, "v2");
  EXPECT_EQ(db_.GetAt("k", snap3).state, LookupState::kDeleted);
  EXPECT_EQ(db_.GetAt("k", 0).state, LookupState::kNotFound);
}

TEST_F(ApiTest, SequenceNumbersAreConsecutive) {
  EXPECT_EQ(db_.LastSequence(), 0u);
  db_.Put("a", "1");
  db_.Delete("b");
  db_.Put("a", "2");
  EXPECT_EQ(db_.LastSequence(), 3u);
}

TEST_F(ApiTest, ManyKeysIndependent) {
  for (int i = 0; i < 500; ++i) db_.Put("key-" + std::to_string(i), "val-" + std::to_string(i));
  for (int i = 0; i < 500; i += 2) db_.Delete("key-" + std::to_string(i));
  for (int i = 0; i < 500; ++i) {
    auto v = db_.Get("key-" + std::to_string(i));
    if (i % 2 == 0) EXPECT_FALSE(v.has_value()) << i;
    else EXPECT_EQ(v, std::optional<std::string>("val-" + std::to_string(i))) << i;
  }
}

TEST_F(ApiTest, RejectsNullMemTable) {
  EXPECT_THROW(MemTableBackend(nullptr), std::invalid_argument);
}

TEST_F(ApiTest, ConcurrentWritersAndReaders) {
  constexpr int kWriters = 4, kReaders = 4, kPerWriter = 2000;
  std::atomic<bool> stop{false};
  std::atomic<int> bad_reads{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      while (!stop) {
        for (int w = 0; w < kWriters; ++w) {
          // Each writer writes value "<w>:<i>" with increasing i; a reader must never
          // see a value that goes backwards for one key within a single thread.
          auto v = db_.Get("w" + std::to_string(w));
          if (v && v->rfind(std::to_string(w) + ":", 0) != 0) ++bad_reads;
        }
      }
    });
  }
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) db_.Put("w" + std::to_string(w), std::to_string(w) + ":" + std::to_string(i));
    });
  }
  for (auto& t : writers) t.join();
  stop = true;
  for (auto& t : readers) t.join();

  EXPECT_EQ(bad_reads.load(), 0);
  EXPECT_EQ(db_.LastSequence(), static_cast<SequenceNumber>(kWriters * kPerWriter));
  for (int w = 0; w < kWriters; ++w) {
    EXPECT_EQ(*db_.Get("w" + std::to_string(w)), std::to_string(w) + ":" + std::to_string(kPerWriter - 1));
  }
}

TEST_F(ApiTest, SnapshotIsStableWhileWritesContinue) {
  db_.Put("k", "before");
  const auto snap = db_.LastSequence();
  std::thread t([&] { for (int i = 0; i < 1000; ++i) db_.Put("k", "after-" + std::to_string(i)); });
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(db_.GetAt("k", snap).value, "before");
  t.join();
}
