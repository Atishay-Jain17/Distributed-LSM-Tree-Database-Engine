// Contract tests for any MemTable implementation. To test the SkipList, add it to
// the INSTANTIATE list at the bottom (a factory returning shared_ptr<MemTable>).
#include <gtest/gtest.h>

#include <functional>
#include <memory>

#include "lsmdb/api/reference_memtable.h"

using namespace lsmdb::api;

namespace {
using Factory = std::function<std::shared_ptr<MemTable>()>;
class MemTableContract : public ::testing::TestWithParam<Factory> {
 protected:
  void SetUp() override { mt_ = GetParam()(); }
  std::shared_ptr<MemTable> mt_;
};
}  // namespace

TEST_P(MemTableContract, MissingKeyIsNotFound) {
  EXPECT_EQ(mt_->Get("nope", 100).state, LookupState::kNotFound);
}

TEST_P(MemTableContract, InsertThenLookup) {
  mt_->Add(1, ValueType::kValue, "k", "v");
  auto r = mt_->Get("k", 1);
  EXPECT_EQ(r.state, LookupState::kFound);
  EXPECT_EQ(r.value, "v");
  EXPECT_EQ(r.sequence, 1u);
}

TEST_P(MemTableContract, NewestVisibleVersionWins) {
  mt_->Add(1, ValueType::kValue, "k", "v1");
  mt_->Add(5, ValueType::kValue, "k", "v5");
  mt_->Add(9, ValueType::kValue, "k", "v9");
  EXPECT_EQ(mt_->Get("k", 100).value, "v9");
  EXPECT_EQ(mt_->Get("k", 9).value, "v9");
  EXPECT_EQ(mt_->Get("k", 8).value, "v5");
  EXPECT_EQ(mt_->Get("k", 5).value, "v5");
  EXPECT_EQ(mt_->Get("k", 4).value, "v1");
  EXPECT_EQ(mt_->Get("k", 0).state, LookupState::kNotFound);  // before first write
}

TEST_P(MemTableContract, TombstoneHidesOlderValueButNotNewer) {
  mt_->Add(1, ValueType::kValue, "k", "old");
  mt_->Add(2, ValueType::kTombstone, "k", "");
  mt_->Add(3, ValueType::kValue, "k", "new");
  EXPECT_EQ(mt_->Get("k", 1).state, LookupState::kFound);
  EXPECT_EQ(mt_->Get("k", 2).state, LookupState::kDeleted);
  EXPECT_EQ(mt_->Get("k", 3).value, "new");
}

TEST_P(MemTableContract, KeysAreIndependentAndPrefixSafe) {
  mt_->Add(1, ValueType::kValue, "a", "1");
  mt_->Add(2, ValueType::kValue, "ab", "2");
  mt_->Add(3, ValueType::kTombstone, "b", "");
  EXPECT_EQ(mt_->Get("a", 10).value, "1");
  EXPECT_EQ(mt_->Get("ab", 10).value, "2");
  EXPECT_EQ(mt_->Get("abc", 10).state, LookupState::kNotFound);
  EXPECT_EQ(mt_->Get("b", 10).state, LookupState::kDeleted);
}

TEST_P(MemTableContract, EmptyValueIsNotMissing) {
  mt_->Add(1, ValueType::kValue, "k", "");
  auto r = mt_->Get("k", 1);
  EXPECT_EQ(r.state, LookupState::kFound);
  EXPECT_EQ(r.value, "");
}

TEST_P(MemTableContract, BinaryKeysAndValues) {
  const std::string key("a\0b", 3), val("\0\xff\0", 3);
  mt_->Add(1, ValueType::kValue, key, val);
  EXPECT_EQ(mt_->Get(key, 1).value, val);
  EXPECT_EQ(mt_->Get("a", 1).state, LookupState::kNotFound);
}

TEST_P(MemTableContract, SizeAndCountGrow) {
  EXPECT_EQ(mt_->EntryCount(), 0u);
  auto before = mt_->ApproximateMemoryUsage();
  mt_->Add(1, ValueType::kValue, "k", std::string(100, 'x'));
  mt_->Add(2, ValueType::kValue, "k", std::string(100, 'y'));  // new version = new entry
  EXPECT_EQ(mt_->EntryCount(), 2u);
  EXPECT_GE(mt_->ApproximateMemoryUsage(), before + 200);
}

INSTANTIATE_TEST_SUITE_P(Impls, MemTableContract,
                         ::testing::Values(Factory([] { return std::make_shared<ReferenceMemTable>(); })));
