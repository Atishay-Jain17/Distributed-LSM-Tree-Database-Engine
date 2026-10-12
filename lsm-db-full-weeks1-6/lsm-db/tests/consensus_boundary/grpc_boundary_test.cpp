// Command serialization across real gRPC (Week 5). A client Put/Delete travels over gRPC into the
// node's KvService, through ReplicatedBackend -> ReferenceConsensus -> StorageStateMachine -> storage.
// Built only when the RPC library is part of the build.
#include <gtest/gtest.h>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/consensus_boundary/reference_consensus.h"
#include "lsmdb/consensus_boundary/replicated_backend.h"
#include "lsmdb/consensus_boundary/storage_state_machine.h"
#include "lsmdb/rpc/status.h"
#include "lsmdb/rpc/validation.h"
#include "lsmdb/v1/kv.pb.h"
#include "test_support.h"

using namespace lsmdb;
using lsmdb::testing_support::TestNode;

// The consensus layer keeps its own copy of the limits so it does not depend on gRPC.
static_assert(consensus_boundary::kMaxKeyBytes == rpc::kMaxKeyBytes);
static_assert(consensus_boundary::kMaxValueBytes == rpc::kMaxValueBytes);
static_assert(consensus_boundary::kMaxRequestIdBytes == rpc::kMaxRequestIdBytes);
// The largest possible command must fit in one gRPC message (AppendEntries carries it as bytes).
static_assert(consensus_boundary::kMaxEncodedCommandBytes < static_cast<std::size_t>(rpc::kMaxMessageBytes));

namespace {

struct Cluster1 {
  std::shared_ptr<api::MemTableBackend> storage =
      std::make_shared<api::MemTableBackend>(std::make_shared<api::ReferenceMemTable>());
  std::shared_ptr<consensus_boundary::StorageStateMachine> sm =
      std::make_shared<consensus_boundary::StorageStateMachine>(storage);
  std::shared_ptr<consensus_boundary::ReferenceConsensus> raft;
  std::unique_ptr<TestNode> node;
  std::unique_ptr<rpc::KvClient> client;

  explicit Cluster1(consensus_boundary::ReferenceConsensusOptions o = {}) {
    raft = std::make_shared<consensus_boundary::ReferenceConsensus>(sm, o);
    auto backend = std::make_shared<consensus_boundary::ReplicatedBackend>(raft, storage);
    node = std::make_unique<TestNode>(1, std::vector<v1::NodeInfo>{}, backend);
    client = std::make_unique<rpc::KvClient>(node->channel());
  }
};

}  // namespace

TEST(GrpcBoundary, ClientWritesBecomeLogEntriesThatDecodeToTheSameCommands) {
  Cluster1 c;
  ASSERT_TRUE(rpc::IsOk(c.client->Put("alpha", "1")));
  ASSERT_TRUE(rpc::IsOk(c.client->Put("alpha", "2")));
  ASSERT_TRUE(rpc::IsOk(c.client->Delete("alpha")));
  ASSERT_TRUE(rpc::IsOk(c.client->Put("beta", std::string("\0\xff\x01", 3))));

  auto log = c.raft->LogSnapshot();
  ASSERT_EQ(log.size(), 4u);
  std::vector<consensus_boundary::Command> got;
  for (const auto& entry : log) {
    consensus_boundary::DecodeResult d = consensus_boundary::Decode(entry);
    ASSERT_TRUE(d) << d.error;
    got.push_back(*d.command);
  }
  EXPECT_EQ(got[0].type, consensus_boundary::CommandType::kPut);
  EXPECT_EQ(got[0].key, "alpha");
  EXPECT_EQ(got[0].value, "1");
  EXPECT_EQ(got[1].value, "2");
  EXPECT_EQ(got[2].type, consensus_boundary::CommandType::kDelete);
  EXPECT_EQ(got[2].key, "alpha");
  EXPECT_EQ(got[3].key, "beta");
  EXPECT_EQ(got[3].value, std::string("\0\xff\x01", 3));

  auto r = c.client->Get("beta");
  ASSERT_TRUE(rpc::IsOk(r.status));
  EXPECT_EQ(r.value, std::string("\0\xff\x01", 3));
  EXPECT_EQ(c.client->Get("alpha").status.code(), v1::STATUS_CODE_NOT_FOUND);
}

TEST(GrpcBoundary, RequestRejectedByRpcValidationNeverReachesTheLog) {
  Cluster1 c;
  v1::Status s = c.client->Put("", "v");  // empty key
  EXPECT_EQ(s.code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(c.raft->LastIndex(), 0u);
}

TEST(GrpcBoundary, MaximumSizeValueCrossesGrpcAndIsLoggedIntact) {
  Cluster1 c;
  std::string big(consensus_boundary::kMaxValueBytes, 'z');
  ASSERT_TRUE(rpc::IsOk(c.client->Put("big", big)));
  auto r = c.client->Get("big");
  ASSERT_TRUE(rpc::IsOk(r.status));
  EXPECT_EQ(r.value.size(), big.size());
  auto log = c.raft->LogSnapshot();
  ASSERT_EQ(log.size(), 1u);
  EXPECT_EQ(log[0].size(), consensus_boundary::kCommandHeaderBytes + 3 + big.size());
  EXPECT_EQ(consensus_boundary::Decode(log[0]).command->value, big);
}

TEST(GrpcBoundary, WriteToAFollowerFailsAndLeavesStorageAndLogUntouched) {
  Cluster1 c({.is_leader = false, .leader_hint = 2});
  v1::Status s = c.client->Put("k", "v");
  EXPECT_FALSE(rpc::IsOk(s));   // today reported as INTERNAL; becomes NOT_LEADER once the RPC layer maps it
  EXPECT_EQ(c.raft->LastIndex(), 0u);
  EXPECT_EQ(c.client->Get("k").status.code(), v1::STATUS_CODE_NOT_FOUND);
}

TEST(GrpcBoundary, EncodedCommandBytesSurviveProtobufWireFormat) {
  // AppendEntries (Atishay's raft.proto) will carry Encode() output in a `bytes` field. PutRequest
  // has the same field type, so it stands in for it here: serialize, parse, decode.
  consensus_boundary::Command in = consensus_boundary::Command::Put(std::string("k\0ey", 4), std::string("\0\0v\xff", 4), "rid");
  v1::PutRequest msg;
  msg.set_key("carrier");
  msg.set_value(consensus_boundary::Encode(in));
  std::string wire;
  ASSERT_TRUE(msg.SerializeToString(&wire));
  v1::PutRequest back;
  ASSERT_TRUE(back.ParseFromString(wire));
  consensus_boundary::DecodeResult d = consensus_boundary::Decode(back.value());
  ASSERT_TRUE(d) << d.error;
  EXPECT_EQ(*d.command, in);
}
