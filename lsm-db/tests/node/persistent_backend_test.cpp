#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lsmdb/node/persistent_backend.h"

namespace fs = std::filesystem;
namespace {
fs::path Temp(const char* name) {
  auto p = fs::temp_directory_path() / (std::string("lsmdb-node-") + name);
  fs::remove_all(p);
  return p;
}
}

TEST(PersistentBackend, ReplaysWritesDeletesAndContinuesSequence) {
  const auto dir = Temp("replay");
  {
    auto db = lsmdb::node::PersistentBackend::Open(dir);
    db->Put("kept", "value");
    db->Put("removed", "old");
    db->Delete("removed");
  }
  {
    auto db = lsmdb::node::PersistentBackend::Open(dir);
    EXPECT_EQ(db->recovery_stats().records_applied, 3u);
    EXPECT_EQ(db->Get("kept"), std::optional<std::string>("value"));
    EXPECT_FALSE(db->Get("removed"));
    db->Put("after-restart", "new");
    EXPECT_EQ(db->store().LastSequence(), 4u);
  }
  fs::remove_all(dir);
}

TEST(PersistentBackend, DiscoversFlushedTablesAfterRestart) {
  const auto dir = Temp("flush");
  {
    auto db = lsmdb::node::PersistentBackend::Open(dir);
    db->Put("key", "value");
    db->Flush();
  }
  {
    auto db = lsmdb::node::PersistentBackend::Open(dir);
    EXPECT_LE(db->recovery_stats().records_skipped, 1u);
    EXPECT_EQ(db->store().LastSequence(), 1u);
    EXPECT_EQ(db->Get("key"), std::optional<std::string>("value"));
  }
  fs::remove_all(dir);
}

TEST(PersistentBackend, TruncatesTornWalTailBeforeAppendingAgain) {
  const auto dir = Temp("tail");
  std::uintmax_t complete_size = 0;
  {
    auto db = lsmdb::node::PersistentBackend::Open(dir);
    db->Put("key", "value");
  }
  complete_size = fs::file_size(dir / "wal.log");
  {
    std::ofstream out(dir / "wal.log", std::ios::binary | std::ios::app);
    out.write("\x01\x02\x03", 3);
  }
  {
    auto db = lsmdb::node::PersistentBackend::Open(dir);
    EXPECT_TRUE(db->recovery_stats().dropped_torn_tail);
    EXPECT_EQ(fs::file_size(dir / "wal.log"), complete_size);
    db->Put("next", "value");
  }
  {
    auto db = lsmdb::node::PersistentBackend::Open(dir);
    EXPECT_EQ(db->Get("key"), std::optional<std::string>("value"));
    EXPECT_EQ(db->Get("next"), std::optional<std::string>("value"));
  }
  fs::remove_all(dir);
}
