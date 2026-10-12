#include "test_framework.h"
#include "storage/memtable.h"
#include <vector>

using namespace lsm;

TEST_CASE(TestMemTableBasicOperations) {
    MemTable memtable;
    ASSERT_TRUE(memtable.Empty());
    ASSERT_EQ(memtable.EntryCount(), 0);

    ASSERT_STATUS_OK(memtable.Put("key1", "val1"));
    ASSERT_STATUS_OK(memtable.Put("key2", "val2"));
    ASSERT_EQ(memtable.EntryCount(), 2);
    ASSERT_FALSE(memtable.Empty());
    ASSERT_TRUE(memtable.ApproximateMemoryUsage() > 0);

    std::string val;
    ASSERT_STATUS_OK(memtable.Get("key1", &val));
    ASSERT_EQ(val, "val1");
    ASSERT_STATUS_OK(memtable.Get("key2", &val));
    ASSERT_EQ(val, "val2");

    // Missing key
    Status s = memtable.Get("missing", &val);
    ASSERT_STATUS_CODE(s, StatusCode::NotFound);

    // Delete
    ASSERT_STATUS_OK(memtable.Delete("key1"));
    ASSERT_EQ(memtable.EntryCount(), 1);
    ASSERT_STATUS_CODE(memtable.Get("key1", &val), StatusCode::NotFound);
    ASSERT_STATUS_CODE(memtable.Delete("key1"), StatusCode::NotFound);
}

TEST_CASE(TestMemTableInputValidation) {
    MemTable memtable;
    ASSERT_STATUS_CODE(memtable.Put("", "val"), StatusCode::InvalidArgument);
    std::string val;
    ASSERT_STATUS_CODE(memtable.Get("", &val), StatusCode::InvalidArgument);
    ASSERT_STATUS_CODE(memtable.Get("key", nullptr), StatusCode::InvalidArgument);
    ASSERT_STATUS_CODE(memtable.Delete(""), StatusCode::InvalidArgument);
}

TEST_CASE(TestMemTableOrderedFlushIteration) {
    MemTable memtable;
    memtable.Put("delta", "4");
    memtable.Put("alpha", "1");
    memtable.Put("charlie", "3");
    memtable.Put("bravo", "2");

    std::vector<std::pair<std::string, std::string>> collected;
    for (auto it = memtable.NewIterator(); it.Valid(); it.Next()) {
        collected.emplace_back(it.Key(), it.Value());
    }

    ASSERT_EQ(collected.size(), 4);
    ASSERT_EQ(collected[0].first, "alpha");
    ASSERT_EQ(collected[0].second, "1");
    ASSERT_EQ(collected[1].first, "bravo");
    ASSERT_EQ(collected[1].second, "2");
    ASSERT_EQ(collected[2].first, "charlie");
    ASSERT_EQ(collected[2].second, "3");
    ASSERT_EQ(collected[3].first, "delta");
    ASSERT_EQ(collected[3].second, "4");
}

TEST_CASE(TestMemTableClear) {
    MemTable memtable;
    for (int i = 0; i < 50; ++i) {
        memtable.Put("k" + std::to_string(i), "v" + std::to_string(i));
    }
    ASSERT_EQ(memtable.EntryCount(), 50);
    ASSERT_TRUE(memtable.ApproximateMemoryUsage() > 0);

    memtable.Clear();
    ASSERT_TRUE(memtable.Empty());
    ASSERT_EQ(memtable.EntryCount(), 0);
    ASSERT_EQ(memtable.ApproximateMemoryUsage(), 0);
}
