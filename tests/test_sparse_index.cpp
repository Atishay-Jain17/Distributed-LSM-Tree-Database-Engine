#include "test_framework.h"
#include "storage/sparse_index.h"
#include "storage/sstable_interface.h"
#include <sstream>

using namespace lsm;

TEST_CASE(TestSparseIndexEmpty) {
    SparseIndex index;
    ASSERT_TRUE(index.Empty());
    ASSERT_EQ(index.EntryCount(), 0);

    IndexEntry entry;
    ASSERT_FALSE(index.FindCandidateBlock("anything", &entry));
}

TEST_CASE(TestSparseIndexNearestEntrySelection) {
    SparseIndex index;
    // Blocks indexed at keys: "b", "d", "f", "h"
    index.AddEntry("b", 0, 1024);
    index.AddEntry("d", 1024, 1024);
    index.AddEntry("f", 2048, 1024);
    index.AddEntry("h", 3072, 1024);

    ASSERT_EQ(index.EntryCount(), 4);

    IndexEntry entry;

    // 1. Key before first index entry: should select block 0
    ASSERT_TRUE(index.FindCandidateBlock("a", &entry));
    ASSERT_EQ(entry.key, "b");
    ASSERT_EQ(entry.offset, 0ULL);

    // 2. Exact match at first entry
    ASSERT_TRUE(index.FindCandidateBlock("b", &entry));
    ASSERT_EQ(entry.key, "b");
    ASSERT_EQ(entry.offset, 0ULL);

    // 3. Key in between "b" and "d": candidate is "b"
    ASSERT_TRUE(index.FindCandidateBlock("c", &entry));
    ASSERT_EQ(entry.key, "b");
    ASSERT_EQ(entry.offset, 0ULL);

    // 4. Exact match at second entry
    ASSERT_TRUE(index.FindCandidateBlock("d", &entry));
    ASSERT_EQ(entry.key, "d");
    ASSERT_EQ(entry.offset, 1024ULL);

    // 5. Key in between "f" and "h": candidate is "f"
    ASSERT_TRUE(index.FindCandidateBlock("g", &entry));
    ASSERT_EQ(entry.key, "f");
    ASSERT_EQ(entry.offset, 2048ULL);

    // 6. Key past last entry: candidate is "h"
    ASSERT_TRUE(index.FindCandidateBlock("z", &entry));
    ASSERT_EQ(entry.key, "h");
    ASSERT_EQ(entry.offset, 3072ULL);
}

TEST_CASE(TestSparseIndexSerialization) {
    SparseIndex index;
    index.AddEntry("alpha", 0, 500);
    index.AddEntry("beta", 500, 600);
    index.AddEntry("gamma", 1100, 700);
    index.SetKeyRange("alpha", "gamma");

    std::stringstream ss;
    index.Serialize(ss);

    SparseIndex restored;
    ASSERT_STATUS_OK(restored.Deserialize(ss));

    ASSERT_EQ(restored.EntryCount(), 3);
    IndexEntry entry;
    ASSERT_TRUE(restored.FindCandidateBlock("beta", &entry));
    ASSERT_EQ(entry.key, "beta");
    ASSERT_EQ(entry.offset, 500ULL);
    ASSERT_EQ(entry.size, 600U);
    ASSERT_FALSE(restored.MayContainRange("aardvark"));
    ASSERT_TRUE(restored.MayContainRange("gamma"));
    ASSERT_FALSE(restored.MayContainRange("zulu"));
}

TEST_CASE(TestSparseIndexRejectsUnorderedEntries) {
    SparseIndex index;
    index.AddEntry("b", 0, 10);
    bool threw = false;
    try { index.AddEntry("a", 10, 10); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
}

TEST_CASE(TestMultiTableLookup) {
    // Simulate 3 SSTables from leveled storage
    IndexedBlockTableMock table1(1);
    table1.AddBlock("k00", 0, 500, {{"k00", "v00"}, {"k01", "v01"}, {"k02", "v02"}});
    table1.AddBlock("k03", 500, 500, {{"k03", "v03"}, {"k04", "v04"}, {"k05", "v05"}});

    IndexedBlockTableMock table2(2);
    table2.AddBlock("k10", 0, 500, {{"k10", "v10"}, {"k11", "v11"}});
    table2.AddBlock("k12", 500, 500, {{"k12", "v12"}, {"k13", "v13"}});

    std::vector<const ISSTableReader*> tables = {&table1, &table2};

    MultiTableReader multi_reader(tables);

    std::string val;
    ASSERT_STATUS_OK(multi_reader.Get("k01", &val));
    ASSERT_EQ(val, "v01");

    ASSERT_STATUS_OK(multi_reader.Get("k04", &val));
    ASSERT_EQ(val, "v04");

    ASSERT_STATUS_OK(multi_reader.Get("k13", &val));
    ASSERT_EQ(val, "v13");

    ASSERT_STATUS_CODE(multi_reader.Get("k99", &val), StatusCode::NotFound);
}
