#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lsmdb/lifecycle/lsm_store.h"
#include "lsmdb/lifecycle/reference_table_store.h"
#include "lsmdb/api/reference_memtable.h"

namespace fs = std::filesystem;
using namespace lsmdb::lifecycle;
using lsmdb::api::LookupState;

namespace {
fs::path DetermineCompactionTestDir() {
    const char* env_temp = std::getenv("LSMDB_TEST_TEMP");
    if (env_temp && *env_temp) return fs::path(env_temp) / "lsmdb-compaction-unit-test";
    const char* temp_env = std::getenv("TEMP");
    if (temp_env && *temp_env) return fs::path(temp_env) / "lsmdb-compaction-unit-test";
    return fs::current_path() / "build" / "test-data" / "lsmdb-compaction-unit-test";
}
} // namespace

TEST_CASE(TestTableStoreFlushesAndManifestRecovery) {
    fs::path dir = DetermineCompactionTestDir() / "recovery";
    std::error_code ec;
    fs::remove_all(dir, ec);

    {
        auto tables = std::make_shared<ReferenceTableStore>(dir, ReferenceTableStoreOptions{10});
        lsmdb::api::ReferenceMemTable mem1;
        mem1.Add(1, lsmdb::api::ValueType::kValue, "user:1", "Alice");
        mem1.Add(2, lsmdb::api::ValueType::kValue, "user:2", "Bob");
        tables->AddTable(mem1);

        lsmdb::api::ReferenceMemTable mem2;
        mem2.Add(3, lsmdb::api::ValueType::kValue, "user:1", "AliceUpdated");
        mem2.Add(4, lsmdb::api::ValueType::kTombstone, "user:2", "");
        tables->AddTable(mem2);

        ASSERT_EQ(tables->TableCount(), 2ULL);
        ASSERT_EQ(tables->LastSequence(), 4ULL);
    }

    // Reopen from disk files and manifest
    {
        auto tables = std::make_shared<ReferenceTableStore>(dir, ReferenceTableStoreOptions{10});
        ASSERT_EQ(tables->TableCount(), 2ULL);
        ASSERT_EQ(tables->LastSequence(), 4ULL);

        auto res1 = tables->Get("user:1", 4);
        ASSERT_TRUE(res1.state == LookupState::kFound);
        ASSERT_EQ(res1.value, "AliceUpdated");

        auto res2 = tables->Get("user:2", 4);
        ASSERT_TRUE(res2.state == LookupState::kDeleted);
    }

    fs::remove_all(dir, ec);
}

TEST_CASE(TestTableStoreLeveledCompactionAndTombstonePurge) {
    fs::path dir = DetermineCompactionTestDir() / "compaction";
    std::error_code ec;
    fs::remove_all(dir, ec);

    auto tables = std::make_shared<ReferenceTableStore>(dir, ReferenceTableStoreOptions{2});
    LsmOptions opt;
    opt.memtable_bytes = 256;
    LsmStore store(tables, opt);

    // Write enough data to force multiple MemTable rotations and flushes to table files
    for (int i = 0; i < 60; ++i) {
        store.Put("key:" + std::to_string(i), "value:" + std::to_string(i));
    }
    // Delete half of them
    for (int i = 0; i < 30; ++i) {
        store.Delete("key:" + std::to_string(i));
    }
    store.Flush();
    store.WaitForIdle();

    // Verify compaction occurred
    ASSERT_TRUE(store.stats().flushes >= 1);
    ASSERT_TRUE(store.stats().compactions >= 1);

    // Verify state correctness after compaction
    for (int i = 0; i < 30; ++i) {
        ASSERT_FALSE(store.Get("key:" + std::to_string(i)).has_value());
    }
    for (int i = 30; i < 60; ++i) {
        auto val = store.Get("key:" + std::to_string(i));
        ASSERT_TRUE(val.has_value());
        ASSERT_EQ(*val, "value:" + std::to_string(i));
    }

    fs::remove_all(dir, ec);
}
