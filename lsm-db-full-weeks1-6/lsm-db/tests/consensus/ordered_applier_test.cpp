#include <gtest/gtest.h>

#include <mutex>
#include <stdexcept>
#include <thread>

#include "lsmdb/consensus/command.h"
#include "lsmdb/consensus/ordered_applier.h"

using namespace lsmdb;
using namespace lsmdb::consensus;
using std::chrono::milliseconds;

namespace {
// Records the order in which commands reach storage.
class RecordingBackend : public rpc::InMemoryBackend {
 public:
  void Put(const std::string& k, const std::string& v) override {
    { std::lock_guard<std::mutex> l(mu); ops.push_back("put:" + k); }
    InMemoryBackend::Put(k, v);
  }
  void Delete(const std::string& k) override {
    { std::lock_guard<std::mutex> l(mu); ops.push_back("del:" + k); }
    InMemoryBackend::Delete(k);
  }
  std::vector<std::string> Ops() { std::lock_guard<std::mutex> l(mu); return ops; }
  std::mutex mu;
  std::vector<std::string> ops;
};

class ThrowingBackend : public rpc::StorageBackend {
 public:
  void Put(const std::string&, const std::string&) override { throw std::runtime_error("disk full"); }
  std::optional<std::string> Get(const std::string&) override { return std::nullopt; }
  void Delete(const std::string&) override {}
};

std::string P(const std::string& k, const std::string& v) { return EncodeCommand(MakePutCommand(k, v)); }
}  // namespace

TEST(Command, RoundTripAndValidation) {
  auto put = DecodeCommand(EncodeCommand(MakePutCommand(std::string("k\0y", 3), std::string("\0v", 2))));
  ASSERT_TRUE(put.has_value());
  EXPECT_EQ(put->type(), v1::COMMAND_TYPE_PUT);
  EXPECT_EQ(put->key(), std::string("k\0y", 3));
  EXPECT_EQ(put->value(), std::string("\0v", 2));

  auto del = DecodeCommand(EncodeCommand(MakeDeleteCommand("k")));
  ASSERT_TRUE(del.has_value());
  EXPECT_EQ(del->type(), v1::COMMAND_TYPE_DELETE);

  EXPECT_FALSE(DecodeCommand("").has_value());                                   // empty = UNSPECIFIED
  EXPECT_FALSE(DecodeCommand(std::string("\xff\xff\xff", 3)).has_value());        // garbage
  EXPECT_FALSE(DecodeCommand(EncodeCommand(MakePutCommand("", "v"))).has_value());  // empty key
}

TEST(OrderedApplier, AppliesInOrder) {
  auto backend = std::make_shared<RecordingBackend>();
  OrderedApplier a(backend);
  a.OnCommitted(1, P("a", "1"));
  a.OnCommitted(2, P("b", "2"));
  a.OnCommitted(3, EncodeCommand(MakeDeleteCommand("a")));
  EXPECT_EQ(a.LastApplied(), 3u);
  EXPECT_EQ(backend->Ops(), (std::vector<std::string>{"put:a", "put:b", "del:a"}));
  EXPECT_FALSE(backend->Get("a").has_value());
  EXPECT_EQ(*backend->Get("b"), "2");
}

TEST(OrderedApplier, BuffersOutOfOrderEntriesUntilTheGapFills) {
  auto backend = std::make_shared<RecordingBackend>();
  OrderedApplier a(backend);
  a.OnCommitted(3, P("c", "3"));
  a.OnCommitted(2, P("b", "2"));
  EXPECT_EQ(a.LastApplied(), 0u);            // nothing applied: index 1 is missing
  EXPECT_TRUE(backend->Ops().empty());
  a.OnCommitted(1, P("a", "1"));
  EXPECT_EQ(a.LastApplied(), 3u);
  EXPECT_EQ(backend->Ops(), (std::vector<std::string>{"put:a", "put:b", "put:c"}));
}

TEST(OrderedApplier, IgnoresDuplicates) {
  auto backend = std::make_shared<RecordingBackend>();
  OrderedApplier a(backend);
  a.OnCommitted(1, P("a", "1"));
  a.OnCommitted(1, P("a", "1"));        // redelivered after it was applied
  a.OnCommitted(2, P("b", "2"));
  a.OnCommitted(1, P("a", "DIFFERENT"));  // stale redelivery must not overwrite
  EXPECT_EQ(backend->Ops().size(), 2u);
  EXPECT_EQ(*backend->Get("a"), "1");
}

TEST(OrderedApplier, SkipsCorruptPayloadWithoutBreakingTheSequence) {
  auto backend = std::make_shared<RecordingBackend>();
  OrderedApplier a(backend);
  a.OnCommitted(1, P("a", "1"));
  a.OnCommitted(2, std::string("\xff\xff\xff", 3));
  a.OnCommitted(3, P("c", "3"));
  EXPECT_EQ(a.LastApplied(), 3u);
  EXPECT_EQ(a.skipped_corrupt(), 1u);
  EXPECT_EQ(backend->Ops(), (std::vector<std::string>{"put:a", "put:c"}));
}

TEST(OrderedApplier, SupportsAStartingIndexAfterRestart) {
  auto backend = std::make_shared<RecordingBackend>();
  OrderedApplier a(backend, /*first_index=*/101);
  a.OnCommitted(100, P("old", "x"));  // already covered by a snapshot/recovery
  a.OnCommitted(101, P("new", "y"));
  EXPECT_EQ(a.LastApplied(), 101u);
  EXPECT_EQ(backend->Ops(), (std::vector<std::string>{"put:new"}));
}

TEST(OrderedApplier, WaitForAppliedTimesOutThenSucceeds) {
  auto backend = std::make_shared<RecordingBackend>();
  OrderedApplier a(backend);
  EXPECT_FALSE(a.WaitForApplied(1, milliseconds(50)));
  std::thread t([&] { std::this_thread::sleep_for(milliseconds(50)); a.OnCommitted(1, P("a", "1")); });
  EXPECT_TRUE(a.WaitForApplied(1, std::chrono::seconds(5)));
  t.join();
}

TEST(OrderedApplier, StorageFailureStopsApplyingAndReleasesWaiters) {
  OrderedApplier a(std::make_shared<ThrowingBackend>());
  a.OnCommitted(1, P("a", "1"));
  EXPECT_EQ(a.LastApplied(), 0u);                      // did not advance past the failed entry
  EXPECT_NE(a.apply_error().find("disk full"), std::string::npos);
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(a.WaitForApplied(1, std::chrono::seconds(5)));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));  // returned early
  a.OnCommitted(2, EncodeCommand(MakeDeleteCommand("x")));
  EXPECT_EQ(a.LastApplied(), 0u);                      // stays stopped
}

TEST(OrderedApplier, ConcurrentDeliveryStillAppliesStrictlyInIndexOrder) {
  auto backend = std::make_shared<RecordingBackend>();
  OrderedApplier a(backend);
  constexpr int kN = 400, kThreads = 4;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = kN; i >= 1; --i) {                   // each thread sends its share of indices, newest first
        if (i % kThreads == t) a.OnCommitted(i, P("k" + std::to_string(i), "v"));
      }
    });
  }
  for (auto& th : threads) th.join();
  ASSERT_EQ(a.LastApplied(), static_cast<std::uint64_t>(kN));
  auto ops = backend->Ops();
  ASSERT_EQ(ops.size(), static_cast<std::size_t>(kN));
  for (int i = 1; i <= kN; ++i) EXPECT_EQ(ops[i - 1], "put:k" + std::to_string(i));
}
