#include "test_framework.h"
#include "storage/skiplist.h"
#include <vector>
#include <string>
#include <algorithm>

using namespace lsm;

TEST_CASE(TestSkipListEmpty) {
    SkipList list;
    ASSERT_TRUE(list.Empty());
    ASSERT_EQ(list.EntryCount(), 0);
    ASSERT_EQ(list.ApproximateBytes(), 0);

    std::string val;
    ASSERT_FALSE(list.Lookup("any_key", &val));
    ASSERT_FALSE(list.Contains("any_key"));

    auto it = list.Begin();
    ASSERT_FALSE(it.Valid());
}

TEST_CASE(TestSkipListInsertAndLookup) {
    SkipList list;
    ASSERT_TRUE(list.Insert("apple", "red"));
    ASSERT_TRUE(list.Insert("banana", "yellow"));
    ASSERT_TRUE(list.Insert("cherry", "dark-red"));

    ASSERT_EQ(list.EntryCount(), 3);
    ASSERT_FALSE(list.Empty());
    ASSERT_TRUE(list.ApproximateBytes() > 0);

    std::string val;
    ASSERT_TRUE(list.Lookup("apple", &val));
    ASSERT_EQ(val, "red");

    ASSERT_TRUE(list.Lookup("banana", &val));
    ASSERT_EQ(val, "yellow");

    ASSERT_TRUE(list.Lookup("cherry", &val));
    ASSERT_EQ(val, "dark-red");

    ASSERT_FALSE(list.Lookup("durian", &val));
    ASSERT_FALSE(list.Contains("durian"));
}

TEST_CASE(TestSkipListOverwriteDuplicateKey) {
    SkipList list;
    ASSERT_TRUE(list.Insert("user:100", "Alice"));
    ASSERT_EQ(list.EntryCount(), 1);

    std::string val;
    ASSERT_TRUE(list.Lookup("user:100", &val));
    ASSERT_EQ(val, "Alice");

    // Overwrite: returns false indicating updated existing key
    ASSERT_FALSE(list.Insert("user:100", "Bob"));
    ASSERT_EQ(list.EntryCount(), 1);

    ASSERT_TRUE(list.Lookup("user:100", &val));
    ASSERT_EQ(val, "Bob");
}

TEST_CASE(TestSkipListDelete) {
    SkipList list;
    list.Insert("k1", "v1");
    list.Insert("k2", "v2");
    list.Insert("k3", "v3");

    ASSERT_EQ(list.EntryCount(), 3);
    ASSERT_TRUE(list.Contains("k2"));

    ASSERT_TRUE(list.Delete("k2"));
    ASSERT_EQ(list.EntryCount(), 2);
    ASSERT_FALSE(list.Contains("k2"));

    // Deleting again should return false
    ASSERT_FALSE(list.Delete("k2"));
    ASSERT_FALSE(list.Delete("k999"));
}

TEST_CASE(TestSkipListOrderedTraversal) {
    SkipList list;
    std::vector<std::string> keys = {"zoo", "ant", "monkey", "bear", "lion", "zebra"};
    for (const auto& k : keys) {
        list.Insert(k, "val_" + k);
    }

    std::vector<std::string> expected_order = keys;
    std::sort(expected_order.begin(), expected_order.end());

    std::vector<std::string> actual_order;
    for (auto it = list.Begin(); it.Valid(); it.Next()) {
        actual_order.push_back(it.Key());
        ASSERT_EQ(it.Value(), "val_" + it.Key());
    }

    ASSERT_EQ(actual_order.size(), expected_order.size());
    for (size_t i = 0; i < expected_order.size(); ++i) {
        ASSERT_EQ(actual_order[i], expected_order[i]);
    }
}

TEST_CASE(TestSkipListSeek) {
    SkipList list;
    list.Insert("10", "v10");
    list.Insert("20", "v20");
    list.Insert("30", "v30");
    list.Insert("40", "v40");

    // Seek exact match
    auto it1 = list.Seek("20");
    ASSERT_TRUE(it1.Valid());
    ASSERT_EQ(it1.Key(), "20");

    // Seek in-between match
    auto it2 = list.Seek("25");
    ASSERT_TRUE(it2.Valid());
    ASSERT_EQ(it2.Key(), "30");

    // Seek before first
    auto it3 = list.Seek("05");
    ASSERT_TRUE(it3.Valid());
    ASSERT_EQ(it3.Key(), "10");

    // Seek past last
    auto it4 = list.Seek("50");
    ASSERT_FALSE(it4.Valid());
}

TEST_CASE(TestSkipListLargeDataset) {
    SkipList list;
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i) {
        // Format with leading zeroes so lexicographical order matches numerical order
        char buf[16];
        snprintf(buf, sizeof(buf), "%05d", i);
        list.Insert(buf, "val" + std::to_string(i));
    }

    ASSERT_EQ(list.EntryCount(), kCount);

    int idx = 0;
    for (auto it = list.Begin(); it.Valid(); it.Next(), ++idx) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%05d", idx);
        ASSERT_EQ(it.Key(), std::string(buf));
    }
    ASSERT_EQ(idx, kCount);
}
