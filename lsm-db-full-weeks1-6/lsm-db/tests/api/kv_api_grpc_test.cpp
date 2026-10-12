// Same API semantics, but through real gRPC: client -> KvService -> MemTableBackend.
#include <gtest/gtest.h>

#include <memory>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/rpc/status.h"
#include "../rpc/test_support.h"

using namespace lsmdb;
using lsmdb::testing_support::TestNode;

namespace {
class GrpcApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    backend_ = std::make_shared<api::MemTableBackend>(std::make_shared<api::ReferenceMemTable>());
    node_ = std::make_unique<TestNode>(1, std::vector<v1::NodeInfo>{}, backend_);
    client_ = std::make_unique<rpc::KvClient>(node_->channel());
  }
  std::shared_ptr<api::MemTableBackend> backend_;
  std::unique_ptr<TestNode> node_;
  std::unique_ptr<rpc::KvClient> client_;
};
}  // namespace

TEST_F(GrpcApiTest, PutGetOverwriteDeleteFlow) {
  EXPECT_TRUE(rpc::IsOk(client_->Put("k", "v1")));
  EXPECT_TRUE(rpc::IsOk(client_->Put("k", "v2")));
  auto r = client_->Get("k");
  EXPECT_TRUE(rpc::IsOk(r.status));
  EXPECT_EQ(r.value, "v2");
  EXPECT_TRUE(rpc::IsOk(client_->Delete("k")));
  EXPECT_EQ(client_->Get("k").status.code(), v1::STATUS_CODE_NOT_FOUND);
  EXPECT_TRUE(rpc::IsOk(client_->Delete("k")));  // idempotent
}

TEST_F(GrpcApiTest, MissingKeyIsNotFoundAndEmptyKeyIsRejected) {
  EXPECT_EQ(client_->Get("absent").status.code(), v1::STATUS_CODE_NOT_FOUND);
  EXPECT_EQ(client_->Put("", "v").code(), v1::STATUS_CODE_INVALID_ARGUMENT);
}
