// Write path without gRPC: handler + applier + fake Raft. Fast, deterministic.
#include <gtest/gtest.h>

#include "lsmdb/consensus/replicated_write_handler.h"
#include "lsmdb/rpc/status.h"
#include "../support/fake_raft.h"

using namespace lsmdb;
using namespace lsmdb::consensus;
using lsmdb::testing_support::FakeRaftCluster;

namespace {
struct Replica {
  std::shared_ptr<rpc::InMemoryBackend> backend = std::make_shared<rpc::InMemoryBackend>();
  std::shared_ptr<OrderedApplier> applier;
  std::unique_ptr<ReplicatedWriteHandler> handler;
};

struct Fixture {
  FakeRaftCluster raft{{1, 2, 3}, 1};
  Replica r[4];  // index 1..3
  Fixture() {
    for (std::uint32_t id = 1; id <= 3; ++id) {
      v1::NodeInfo info;
      info.set_node_id(id);
      info.set_host("10.0.0." + std::to_string(id));
      info.set_port(50050 + id);
      raft.SetAddress(id, info);
      r[id].applier = std::make_shared<OrderedApplier>(r[id].backend);
      r[id].handler = std::make_unique<ReplicatedWriteHandler>(raft.ProposerFor(id), r[id].applier);
    }
  }
};
}  // namespace

TEST(ReplicatedWrite, LeaderPutReachesEveryReplica) {
  Fixture f;
  EXPECT_TRUE(rpc::IsOk(f.r[1].handler->Put("k", "v")));
  for (int id = 1; id <= 3; ++id) {
    ASSERT_TRUE(f.r[id].backend->Get("k").has_value()) << "node " << id;
    EXPECT_EQ(*f.r[id].backend->Get("k"), "v");
    EXPECT_EQ(f.r[id].applier->LastApplied(), 1u);
  }
}

TEST(ReplicatedWrite, DeleteIsReplicatedToo) {
  Fixture f;
  ASSERT_TRUE(rpc::IsOk(f.r[1].handler->Put("k", "v")));
  EXPECT_TRUE(rpc::IsOk(f.r[1].handler->Delete("k")));
  for (int id = 1; id <= 3; ++id) EXPECT_FALSE(f.r[id].backend->Get("k").has_value());
}

TEST(ReplicatedWrite, FollowerRedirectsToLeaderAndChangesNothing) {
  Fixture f;
  auto st = f.r[2].handler->Put("k", "v");
  EXPECT_EQ(st.code(), v1::STATUS_CODE_NOT_LEADER);
  ASSERT_TRUE(st.has_leader_hint());
  EXPECT_EQ(st.leader_hint().node_id(), 1u);
  EXPECT_EQ(st.leader_hint().port(), 50051u);
  EXPECT_EQ(f.raft.log_size(), 0u);
  for (int id = 1; id <= 3; ++id) EXPECT_FALSE(f.r[id].backend->Get("k").has_value());
}

TEST(ReplicatedWrite, SingleFailureStillCommitsWithQuorum) {
  Fixture f;
  f.raft.Kill(3);
  EXPECT_TRUE(rpc::IsOk(f.r[1].handler->Put("k", "v")));       // 2 of 3 alive
  EXPECT_EQ(*f.r[2].backend->Get("k"), "v");
  EXPECT_FALSE(f.r[3].backend->Get("k").has_value());          // dead node missed it...
  f.raft.Revive(3);
  EXPECT_EQ(*f.r[3].backend->Get("k"), "v");                    // ...and catches up
  EXPECT_EQ(f.r[3].applier->LastApplied(), 1u);
}

TEST(ReplicatedWrite, LosingQuorumRejectsWritesAndAppliesNothing) {
  Fixture f;
  ASSERT_TRUE(rpc::IsOk(f.r[1].handler->Put("before", "v")));
  f.raft.Kill(2);
  f.raft.Kill(3);
  auto st = f.r[1].handler->Put("after", "v");
  EXPECT_EQ(st.code(), v1::STATUS_CODE_UNAVAILABLE);
  EXPECT_FALSE(f.r[1].backend->Get("after").has_value());
  EXPECT_EQ(*f.r[1].backend->Get("before"), "v");               // committed data is safe
  EXPECT_EQ(f.raft.log_size(), 1u);
}

TEST(ReplicatedWrite, LeaderChangeKeepsOrderingAndHistory) {
  Fixture f;
  ASSERT_TRUE(rpc::IsOk(f.r[1].handler->Put("a", "1")));
  f.raft.Kill(1);
  f.raft.SetLeader(2);
  ASSERT_TRUE(rpc::IsOk(f.r[2].handler->Put("a", "2")));
  ASSERT_TRUE(rpc::IsOk(f.r[2].handler->Put("b", "3")));
  EXPECT_EQ(*f.r[3].backend->Get("a"), "2");
  EXPECT_EQ(f.r[3].applier->LastApplied(), 3u);
  f.raft.Revive(1);                                              // old leader returns as follower
  EXPECT_EQ(*f.r[1].backend->Get("a"), "2");
  EXPECT_EQ(f.r[1].handler->Put("c", "x").code(), v1::STATUS_CODE_NOT_LEADER);
}

TEST(ReplicatedWrite, CommittedButNotAppliedReportsUnavailable) {
  // A proposer that claims commit at index 5 but never delivers the entry.
  class Stuck : public RaftProposer {
   public:
    ProposalResult Propose(const std::string&, std::chrono::milliseconds) override {
      ProposalResult r;
      r.status = rpc::MakeStatus(v1::STATUS_CODE_OK);
      r.log_index = 5;
      return r;
    }
    void SetCommitCallback(CommitCallback) override {}
  };
  auto backend = std::make_shared<rpc::InMemoryBackend>();
  auto applier = std::make_shared<OrderedApplier>(backend);
  ReplicatedWriteOptions o;
  o.apply_timeout = std::chrono::milliseconds(50);
  ReplicatedWriteHandler handler(std::make_shared<Stuck>(), applier, o);
  auto st = handler.Put("k", "v");
  EXPECT_EQ(st.code(), v1::STATUS_CODE_UNAVAILABLE);
  EXPECT_NE(st.message().find("retrying is safe"), std::string::npos);
}
