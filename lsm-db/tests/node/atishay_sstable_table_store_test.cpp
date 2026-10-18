#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include <unistd.h>

#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/node/atishay_sstable_table_store.h"

namespace fs = std::filesystem;
using namespace lsmdb;

namespace {
fs::path TempDir() {
  auto path = fs::temp_directory_path() / ("lsmdb-indexed-store-" + std::to_string(::getpid()));
  fs::remove_all(path);
  return path;
}

TEST(AtishaySSTableTableStore, FlushesReadsCompactsAndRecoversIndexedTables) {
  const fs::path dir = TempDir();
  {
    node::AtishaySSTableTableStore tables(dir, 3);
    api::ReferenceMemTable first;
    first.Add(1, api::ValueType::kValue, "a", "old");
    first.Add(1, api::ValueType::kValue, "b", "before-delete");
    tables.AddTable(first);

    api::ReferenceMemTable second;
    second.Add(2, api::ValueType::kValue, "a", "middle");
    second.Add(3, api::ValueType::kValue, "a", "new");
    second.Add(4, api::ValueType::kTombstone, "b", "");
    tables.AddTable(second);

    auto current = tables.Get("a", 4);
    ASSERT_EQ(current.state, api::LookupState::kFound);
    EXPECT_EQ(current.value, "new");
    auto snapshot = tables.Get("a", 2);
    ASSERT_EQ(snapshot.state, api::LookupState::kFound);
    EXPECT_EQ(snapshot.value, "middle");
    EXPECT_EQ(tables.Get("b", 2).state, api::LookupState::kFound);
    EXPECT_EQ(tables.Get("b", 4).state, api::LookupState::kDeleted);

    tables.Compact(2);
    EXPECT_EQ(tables.Get("a", 2).value, "middle");
    EXPECT_EQ(tables.Get("a", 4).value, "new");
    EXPECT_EQ(tables.Get("b", 2).value, "before-delete");
    EXPECT_EQ(tables.Get("b", 4).state, api::LookupState::kDeleted);
    EXPECT_EQ(tables.LastSequence(), 4u);
  }
  {
    node::AtishaySSTableTableStore recovered(dir, 3);
    EXPECT_EQ(recovered.LastSequence(), 4u);
    EXPECT_EQ(recovered.Get("a", 2).value, "middle");
    EXPECT_EQ(recovered.Get("a", 4).value, "new");
    EXPECT_EQ(recovered.Get("b", 2).value, "before-delete");
    EXPECT_EQ(recovered.Get("b", 4).state, api::LookupState::kDeleted);
  }
  fs::remove_all(dir);
}
}  // namespace
