// Three real NodeServers over gRPC + ClusterClient + fake Raft: the full Week 6 path.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "lsmdb/consensus/replicated_write_handler.h"
#include "lsmdb/rpc/cluster_client.h"
#include "lsmdb/rpc/node_server.h"
#include "lsmdb/rpc/status.h"
#include "../support/fake_raft.h"

using namespace lsmdb;
using namespace lsmdb::consensus;
using namespace lsmdb::rpc;
using lsmdb::testing_support::FakeRaftCluster;

namespace {
class ThreeNodeCluster : public ::testing::Test {
 protected:
  void SetUp() override {
    raft_ = std::make_unique<FakeRaftCluster>(std::vector<std::uint32_t>{1, 2, 3}, 1);
    for (std::uint32_t id = 1; id <= 3; ++id) {
      auto& n = nodes_[id];
      n.backend = std::make_shared<InMemoryBackend>();
      n.applier = std::make_shared<OrderedApplier>(n.backend);
      n.handler = std::make_shared<ReplicatedWriteHandler>(raft_->ProposerFor(id), n.applier);
      NodeOptions opts;
      opts.node_id = id;
      opts.advertise_host = "127.0.0.1";
      n.server = std::make_unique<NodeServer>(opts, n.backend, n.handler);
      n.server->Start();
      v1::NodeInfo info;
      info.set_node_id(id);
      info.set_host("127.0.0.1");
      info.set_port(n.server->port());
      raft_->SetAddress(id, info);
      targets_.push_back("127.0.0.1:" + std::to_string(n.server->port()));
    }
  }
  void TearDown() override {
    for (auto& [id, n] : nodes_) if (n.server) n.server->Shutdown();
  }
  void Crash(std::uint32_t id) {
    raft_->Kill(id);
    nodes_[id].server->Shutdown();
  }
  KvClient Direct(std::uint32_t id) { return KvClient(MakeChannel(targets_[id - 1])); }

  // Reads are local and replication is asynchronous in real Raft, so poll briefly.
  bool Eventually(std::uint32_t id, const std::string& key, const std::string& want) {
    auto client = Direct(id);
    for (int i = 0; i < 200; ++i) {
      auto r = client.Get(key);
      if (want.empty() ? r.status.code() == v1::STATUS_CODE_NOT_FOUND : (IsOk(r.status) && r.value == want)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  struct N {
    std::shared_ptr<InMemoryBackend> backend;
    std::shared_ptr<OrderedApplier> applier;
    std::shared_ptr<ReplicatedWriteHandler> handler;
    std::unique_ptr<NodeServer> server;
  };
  std::unique_ptr<FakeRaftCluster> raft_;
  std::map<std::uint32_t, N> nodes_;
  std::vector<std::string> targets_;
};
}  // namespace

TEST_F(ThreeNodeCluster, WriteThroughAnyNodeIsReplicatedEverywhere) {
  ClusterClient client({targets_[2], targets_[1], targets_[0]});  // starts at a FOLLOWER
  ASSERT_TRUE(IsOk(client.Put("greeting", "hello")));
  for (std::uint32_t id = 1; id <= 3; ++id) EXPECT_TRUE(Eventually(id, "greeting", "hello")) << "node " << id;
  ASSERT_TRUE(IsOk(client.Delete("greeting")));
  for (std::uint32_t id = 1; id <= 3; ++id) EXPECT_TRUE(Eventually(id, "greeting", "")) << "node " << id;
}

TEST_F(ThreeNodeCluster, FollowerAnswersNotLeaderWithTheLeadersAddress) {
  auto st = Direct(2).Put("k", "v");
  EXPECT_EQ(st.code(), v1::STATUS_CODE_NOT_LEADER);
  ASSERT_TRUE(st.has_leader_hint());
  EXPECT_EQ(st.leader_hint().node_id(), 1u);
  EXPECT_EQ(st.leader_hint().port(), nodes_[1].server->port());
  EXPECT_EQ(Direct(1).Get("k").status.code(), v1::STATUS_CODE_NOT_FOUND);  // nothing was written
}

TEST_F(ThreeNodeCluster, ClientFollowsRedirectAndRemembersTheLeader) {
  ClusterClient client({targets_[1], targets_[2], targets_[0]});
  EXPECT_EQ(client.leader_target(), "");
  ASSERT_TRUE(IsOk(client.Put("k", "v")));
  EXPECT_EQ(client.leader_target(), targets_[0]);
}

TEST_F(ThreeNodeCluster, SurvivesLeaderCrashAndKeepsCommittedData) {
  ClusterClient client(targets_);
  ASSERT_TRUE(IsOk(client.Put("before", "1")));
  ASSERT_EQ(client.leader_target(), targets_[0]);

  Crash(1);
  raft_->SetLeader(2);  // "election finished"

  ASSERT_TRUE(IsOk(client.Put("after", "2")));  // same client object: retries, finds node 2
  EXPECT_EQ(client.leader_target(), targets_[1]);
  for (std::uint32_t id : {2u, 3u}) {
    EXPECT_TRUE(Eventually(id, "before", "1")) << "node " << id;   // old data preserved
    EXPECT_TRUE(Eventually(id, "after", "2")) << "node " << id;    // new write replicated
  }
}

TEST_F(ThreeNodeCluster, WritesFailSafelyWithoutQuorumButReadsStillWork) {
  ClusterClient client(targets_);
  ASSERT_TRUE(IsOk(client.Put("kept", "yes")));
  Crash(2);
  Crash(3);
  ClusterClientOptions fast;
  fast.client.deadline = std::chrono::milliseconds(500);
  fast.retry_backoff = std::chrono::milliseconds(5);
  ClusterClient fast_client({targets_[0]}, fast);
  auto st = fast_client.Put("lost", "no");
  EXPECT_EQ(st.code(), v1::STATUS_CODE_UNAVAILABLE);
  EXPECT_EQ(Direct(1).Get("lost").status.code(), v1::STATUS_CODE_NOT_FOUND);  // never applied
  EXPECT_EQ(Direct(1).Get("kept").value, "yes");                               // still readable
}

TEST_F(ThreeNodeCluster, ConcurrentWritersLeaveAllReplicasIdentical) {
  constexpr int kThreads = 4, kWrites = 40, kKeys = 10;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      ClusterClient client({targets_[t % 3], targets_[(t + 1) % 3], targets_[(t + 2) % 3]});
      for (int i = 0; i < kWrites; ++i) {
        std::string key = "key" + std::to_string(i % kKeys);
        if (!IsOk(client.Put(key, "t" + std::to_string(t) + "-" + std::to_string(i)))) ++failures;
      }
    });
  }
  for (auto& th : threads) th.join();
  ASSERT_EQ(failures, 0);

  const std::uint64_t total = kThreads * kWrites;
  for (std::uint32_t id = 1; id <= 3; ++id) ASSERT_TRUE(nodes_[id].applier->WaitForApplied(total, std::chrono::seconds(5)));
  for (int k = 0; k < kKeys; ++k) {
    std::string key = "key" + std::to_string(k);
    auto v1_ = *nodes_[1].backend->Get(key);
    EXPECT_EQ(*nodes_[2].backend->Get(key), v1_) << key;   // same final value on every node
    EXPECT_EQ(*nodes_[3].backend->Get(key), v1_) << key;   // => same order everywhere
  }
}

TEST_F(ThreeNodeCluster, InvalidRequestsNeverReachRaft) {
  ClusterClient client(targets_);
  EXPECT_EQ(client.Put("", "v").code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(raft_->log_size(), 0u);
}
