#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/consensus/ordered_applier.h"
#include "lsmdb/consensus/replicated_write_handler.h"
#include "lsmdb/reads/consistency_level.h"
#include "lsmdb/reads/consistent_backend.h"
#include "lsmdb/reads/consistent_cluster_client.h"
#include "lsmdb/reads/leader_lease.h"
#include "lsmdb/reads/read_router.h"
#include "lsmdb/rpc/node_server.h"
#include "lsmdb/rpc/storage_backend.h"
#include "../support/fake_raft.h"

namespace lsmdb::reads::testing {

class ReadRouterUnitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    storage_ = std::make_shared<rpc::InMemoryBackend>();
    lease_ = std::make_shared<MockLeaderLeaseProvider>(true, 1, 0);
    v1::NodeInfo hint;
    hint.set_node_id(1);
    hint.set_host("127.0.0.1");
    hint.set_port(50051);
    lease_->SetLeaderHint(hint);
    router_ = std::make_unique<ReadRouter>(storage_, lease_, ConsistencyLevel::kLinearizable);
  }

  std::shared_ptr<rpc::InMemoryBackend> storage_;
  std::shared_ptr<MockLeaderLeaseProvider> lease_;
  std::unique_ptr<ReadRouter> router_;
};

TEST_F(ReadRouterUnitTest, LeaderServesCommittedData) {
  storage_->Put("key1", "val1");
  auto res = router_->Read("key1");
  EXPECT_EQ(res.status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(res.value, "val1");
}

TEST_F(ReadRouterUnitTest, FollowerRejectsReadWithNotLeaderAndHint) {
  lease_->SetLeader(false);
  auto res = router_->Read("key1");
  EXPECT_EQ(res.status.code(), v1::STATUS_CODE_NOT_LEADER);
  EXPECT_TRUE(res.status.has_leader_hint());
  EXPECT_EQ(res.status.leader_hint().node_id(), 1u);
  EXPECT_EQ(res.status.leader_hint().port(), 50051u);
}

TEST_F(ReadRouterUnitTest, LeaderWithoutQuorumFailsWithUnavailable) {
  storage_->Put("key1", "val1");
  lease_->SetQuorumOk(false);  // Quorum unreachable (e.g. partition)
  auto res = router_->Read("key1");
  EXPECT_EQ(res.status.code(), v1::STATUS_CODE_UNAVAILABLE);
  EXPECT_TRUE(res.value.empty());
}

TEST_F(ReadRouterUnitTest, StaleTolerantModeAllowsFollowerReads) {
  storage_->Put("key1", "val1");
  lease_->SetLeader(false);
  router_->SetConsistencyLevel(ConsistencyLevel::kStaleTolerant);

  auto res = router_->Read("key1");
  EXPECT_EQ(res.status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(res.value, "val1");
}

TEST_F(ReadRouterUnitTest, ReadAfterCommittedWriteAndVersionUpdate) {
  // Read-after-write
  storage_->Put("mvcc_key", "version_1");
  auto r1 = router_->Read("mvcc_key");
  EXPECT_EQ(r1.status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(r1.value, "version_1");

  // Version update
  storage_->Put("mvcc_key", "version_2");
  auto r2 = router_->Read("mvcc_key");
  EXPECT_EQ(r2.status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(r2.value, "version_2");

  // Read after delete
  storage_->Delete("mvcc_key");
  auto r3 = router_->Read("mvcc_key");
  EXPECT_EQ(r3.status.code(), v1::STATUS_CODE_NOT_FOUND);
}

TEST_F(ReadRouterUnitTest, ConsistentStorageBackendThrowsOnNotLeader) {
  ConsistentStorageBackend backend(storage_, lease_, ConsistencyLevel::kLinearizable);
  storage_->Put("k", "v");
  EXPECT_EQ(backend.Get("k"), "v");

  lease_->SetLeader(false);
  EXPECT_THROW(backend.Get("k"), ReadNotLeaderException);
}

// ---------------------------------------------------------------------------
// 3-Node Cluster Integration Test
// ---------------------------------------------------------------------------
class ThreeNodeReadClusterTest : public ::testing::Test {
 protected:
  struct ClusterNode {
    std::uint32_t id;
    std::shared_ptr<rpc::InMemoryBackend> raw_storage;
    std::shared_ptr<MockLeaderLeaseProvider> lease;
    std::shared_ptr<ConsistentStorageBackend> backend;
    std::shared_ptr<consensus::OrderedApplier> applier;
    std::shared_ptr<consensus::ReplicatedWriteHandler> write_handler;
    std::unique_ptr<rpc::NodeServer> server;
    std::string address;
  };

  void SetUp() override {
    fake_raft_ = std::make_unique<testing_support::FakeRaftCluster>(
        std::vector<std::uint32_t>{1, 2, 3}, 1);

    for (std::uint32_t id = 1; id <= 3; ++id) {
      auto node = std::make_unique<ClusterNode>();
      node->id = id;
      node->raw_storage = std::make_shared<rpc::InMemoryBackend>();
      node->lease = std::make_shared<MockLeaderLeaseProvider>(id == 1, 1, 0);

      v1::NodeInfo leader_info;
      leader_info.set_node_id(1);
      leader_info.set_host("127.0.0.1");
      node->lease->SetLeaderHint(leader_info);

      node->backend = std::make_shared<ConsistentStorageBackend>(
          node->raw_storage, node->lease, ConsistencyLevel::kLinearizable);

      node->applier = std::make_shared<consensus::OrderedApplier>(node->raw_storage);
      auto proposer = fake_raft_->ProposerFor(id);
      node->write_handler = std::make_shared<consensus::ReplicatedWriteHandler>(
          proposer, node->applier);

      rpc::NodeOptions opts;
      opts.node_id = id;
      opts.port = 0;  // Ephemeral port
      opts.advertise_host = "127.0.0.1";
      node->server = std::make_unique<rpc::NodeServer>(opts, node->backend, node->write_handler);
      node->server->Start();
      node->address = "127.0.0.1:" + std::to_string(node->server->port());

      v1::NodeInfo addr;
      addr.set_node_id(id);
      addr.set_host("127.0.0.1");
      addr.set_port(node->server->port());
      fake_raft_->SetAddress(id, addr);

      nodes_.push_back(std::move(node));
    }

    // Update leader hints with real ports
    for (auto& n : nodes_) {
      v1::NodeInfo hint;
      hint.set_node_id(1);
      hint.set_host("127.0.0.1");
      hint.set_port(nodes_[0]->server->port());
      n->lease->SetLeaderHint(hint);
    }
  }

  void TearDown() override {
    for (auto& n : nodes_) {
      if (n->server) n->server->Shutdown();
    }
  }

  std::vector<std::string> TargetAddresses() const {
    std::vector<std::string> targets;
    for (const auto& n : nodes_) targets.push_back(n->address);
    return targets;
  }

  std::unique_ptr<testing_support::FakeRaftCluster> fake_raft_;
  std::vector<std::unique_ptr<ClusterNode>> nodes_;
};

TEST_F(ThreeNodeReadClusterTest, ReadAfterCommittedWriteAndFollowerRedirect) {
  ConsistentClusterClient client(TargetAddresses());

  // 1. Write through leader
  auto put_status = client.Put("cluster_key", "initial_val");
  EXPECT_EQ(put_status.code(), v1::STATUS_CODE_OK);

  // 2. Read through client (routes to leader)
  auto get_res = client.Get("cluster_key");
  EXPECT_EQ(get_res.status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(get_res.value, "initial_val");

  // 3. Direct read on follower (Node 2) directly via KvClient
  rpc::KvClient follower_client(rpc::MakeChannel(nodes_[1]->address));
  auto f_res = follower_client.Get("cluster_key");
  // Follower cannot serve linearizable read; returns NOT_LEADER with hint
  EXPECT_EQ(f_res.status.code(), v1::STATUS_CODE_NOT_LEADER);
  EXPECT_TRUE(f_res.status.has_leader_hint());
  EXPECT_EQ(f_res.status.leader_hint().node_id(), 1u);
  EXPECT_EQ(f_res.status.leader_hint().port(), nodes_[0]->server->port());
}

TEST_F(ThreeNodeReadClusterTest, ReadsRouteAroundLeaderChange) {
  ConsistentClusterClient client(TargetAddresses());
  ASSERT_EQ(client.Put("leader_change_key", "value_before_failover").code(), v1::STATUS_CODE_OK);

  // Crash leader (Node 1)
  fake_raft_->Kill(1);
  nodes_[0]->lease->SetQuorumOk(false);
  nodes_[0]->lease->SetLeader(false);

  // Promote Node 2 as new leader
  fake_raft_->SetLeader(2);
  nodes_[1]->lease->SetLeader(true);
  nodes_[1]->lease->SetTerm(2);

  v1::NodeInfo hint2;
  hint2.set_node_id(2);
  hint2.set_host("127.0.0.1");
  hint2.set_port(nodes_[1]->server->port());
  for (auto& n : nodes_) n->lease->SetLeaderHint(hint2);

  // ConsistentClusterClient should failover and read from new leader (Node 2)
  auto res = client.Get("leader_change_key");
  EXPECT_EQ(res.status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(res.value, "value_before_failover");
  EXPECT_EQ(client.leader_target(), nodes_[1]->address);

  // Perform write on new leader
  ASSERT_EQ(client.Put("leader_change_key", "value_after_failover").code(), v1::STATUS_CODE_OK);
  auto res2 = client.Get("leader_change_key");
  EXPECT_EQ(res2.status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(res2.value, "value_after_failover");
}

TEST_F(ThreeNodeReadClusterTest, ReadAfterDeleteAcrossCluster) {
  ConsistentClusterClient client(TargetAddresses());
  ASSERT_EQ(client.Put("del_key", "temp_val").code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(client.Get("del_key").value, "temp_val");

  ASSERT_EQ(client.Delete("del_key").code(), v1::STATUS_CODE_OK);
  auto res = client.Get("del_key");
  EXPECT_EQ(res.status.code(), v1::STATUS_CODE_NOT_FOUND);
}

}  // namespace lsmdb::reads::testing
