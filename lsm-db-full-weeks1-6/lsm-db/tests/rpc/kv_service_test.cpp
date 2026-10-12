// End-to-end tests of KvService over real gRPC against an in-process node.
#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <thread>

#include "lsmdb/rpc/status.h"
#include "lsmdb/rpc/validation.h"
#include "test_support.h"

using namespace lsmdb;
using namespace lsmdb::rpc;
using lsmdb::testing_support::TestNode;

namespace {

class CountingBackend : public InMemoryBackend {
 public:
  void Put(const std::string& k, const std::string& v) override {
    ++puts;
    InMemoryBackend::Put(k, v);
  }
  std::optional<std::string> Get(const std::string& k) override {
    ++gets;
    return InMemoryBackend::Get(k);
  }
  void Delete(const std::string& k) override {
    ++deletes;
    InMemoryBackend::Delete(k);
  }
  std::atomic<int> puts{0}, gets{0}, deletes{0};
};

class ThrowingBackend : public StorageBackend {
 public:
  void Put(const std::string&, const std::string&) override { throw std::runtime_error("disk full"); }
  std::optional<std::string> Get(const std::string&) override { throw std::runtime_error("disk full"); }
  void Delete(const std::string&) override { throw std::runtime_error("disk full"); }
};

class KvServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    backend_ = std::make_shared<CountingBackend>();
    node_ = std::make_unique<TestNode>(1, std::vector<v1::NodeInfo>{}, backend_);
    client_ = std::make_unique<KvClient>(node_->channel());
  }
  std::shared_ptr<CountingBackend> backend_;
  std::unique_ptr<TestNode> node_;
  std::unique_ptr<KvClient> client_;
};

}  // namespace

TEST_F(KvServiceTest, PutThenGet) {
  EXPECT_TRUE(IsOk(client_->Put("name", "vinayak")));
  auto r = client_->Get("name");
  EXPECT_TRUE(IsOk(r.status));
  EXPECT_EQ(r.value, "vinayak");
}

TEST_F(KvServiceTest, OverwriteReturnsLatestValue) {
  ASSERT_TRUE(IsOk(client_->Put("k", "v1")));
  ASSERT_TRUE(IsOk(client_->Put("k", "v2")));
  EXPECT_EQ(client_->Get("k").value, "v2");
}

TEST_F(KvServiceTest, MissingKeyIsNotFoundNotAnError) {
  auto r = client_->Get("absent");
  EXPECT_EQ(r.status.code(), v1::STATUS_CODE_NOT_FOUND);
  EXPECT_TRUE(r.value.empty());
}

TEST_F(KvServiceTest, DeleteRemovesKeyAndIsIdempotent) {
  ASSERT_TRUE(IsOk(client_->Put("k", "v")));
  EXPECT_TRUE(IsOk(client_->Delete("k")));
  EXPECT_EQ(client_->Get("k").status.code(), v1::STATUS_CODE_NOT_FOUND);
  EXPECT_TRUE(IsOk(client_->Delete("k")));       // already gone
  EXPECT_TRUE(IsOk(client_->Delete("never")));   // never existed
}

TEST_F(KvServiceTest, EmptyValueRoundTrips) {
  ASSERT_TRUE(IsOk(client_->Put("k", "")));
  auto r = client_->Get("k");
  EXPECT_TRUE(IsOk(r.status));
  EXPECT_EQ(r.value, "");
}

TEST_F(KvServiceTest, BinaryKeysAndValuesSurviveTheWire) {
  std::string key("k\0\xff", 3), value("\0\x01\0\x02", 4);
  ASSERT_TRUE(IsOk(client_->Put(key, value)));
  EXPECT_EQ(client_->Get(key).value, value);
}

TEST_F(KvServiceTest, InvalidRequestsNeverReachStorage) {
  EXPECT_EQ(client_->Put("", "v").code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(client_->Get("").status.code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(client_->Delete("").code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(client_->Put(std::string(kMaxKeyBytes + 1, 'k'), "v").code(),
            v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(client_->Put("k", std::string(kMaxValueBytes + 1, 'v')).code(),
            v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(backend_->puts, 0);
  EXPECT_EQ(backend_->gets, 0);
  EXPECT_EQ(backend_->deletes, 0);
}

TEST_F(KvServiceTest, MaxSizeValueIsAccepted) {
  std::string value(kMaxValueBytes, 'x');
  ASSERT_TRUE(IsOk(client_->Put("big", value)));
  EXPECT_EQ(client_->Get("big").value.size(), kMaxValueBytes);
}

TEST_F(KvServiceTest, MessageAboveTransportLimitMapsToInvalidArgument) {
  // 3 MiB exceeds the 2 MiB gRPC cap, so it is rejected before the handler runs.
  auto st = client_->Put("k", std::string(3 * 1024 * 1024, 'x'));
  EXPECT_EQ(st.code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(backend_->puts, 0);
}

TEST_F(KvServiceTest, OversizedRequestIdIsRejectedByServer) {
  // KvClient always sends a valid id, so use the raw stub to send a bad one.
  auto stub = v1::KvService::NewStub(node_->channel());
  v1::PutRequest req;
  req.mutable_meta()->set_request_id(std::string(kMaxRequestIdBytes + 1, 'x'));
  req.set_key("k");
  v1::PutResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->Put(&ctx, req, &resp).ok());  // transport succeeds...
  EXPECT_EQ(resp.status().code(), v1::STATUS_CODE_INVALID_ARGUMENT);  // ...request is refused
}

TEST_F(KvServiceTest, ConcurrentClientsDoNotLoseWrites) {
  constexpr int kThreads = 8, kPerThread = 50;
  auto channel = node_->channel();
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      KvClient c(channel);
      for (int i = 0; i < kPerThread; ++i) {
        std::string key = "t" + std::to_string(t) + "-k" + std::to_string(i);
        if (!IsOk(c.Put(key, "v" + key))) ++failures;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures, 0);
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      std::string key = "t" + std::to_string(t) + "-k" + std::to_string(i);
      EXPECT_EQ(client_->Get(key).value, "v" + key);
    }
  }
}

TEST(KvServiceFailure, StorageExceptionBecomesInternalStatus) {
  TestNode node(1, {}, std::make_shared<ThrowingBackend>());
  KvClient client(node.channel());
  auto put = client.Put("k", "v");
  EXPECT_EQ(put.code(), v1::STATUS_CODE_INTERNAL);
  EXPECT_NE(put.message().find("disk full"), std::string::npos);
  EXPECT_EQ(client.Get("k").status.code(), v1::STATUS_CODE_INTERNAL);
  EXPECT_EQ(client.Delete("k").code(), v1::STATUS_CODE_INTERNAL);
}

TEST(KvServiceFailure, DeadNodeReportsUnavailable) {
  std::string target;
  {
    TestNode node(1);
    target = node.target();
  }  // server shut down here
  ClientOptions opts;
  opts.deadline = std::chrono::milliseconds(2000);
  KvClient client(MakeChannel(target), opts);
  EXPECT_EQ(client.Put("k", "v").code(), v1::STATUS_CODE_UNAVAILABLE);
  EXPECT_EQ(client.Get("k").status.code(), v1::STATUS_CODE_UNAVAILABLE);
}
