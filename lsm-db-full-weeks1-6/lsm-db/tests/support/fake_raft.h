#pragma once
// ---------------------------------------------------------------------------
// FakeRaftCluster: an in-process stand-in for the real Raft module (Atishay/Sarthak),
// implementing the RaftProposer contract for a small cluster. It lets the whole write
// path (routing, quorum rule, ordered apply, leader failover) be tested before the real
// Raft exists, and the SAME tests can later run against the real implementation.
//
// Model: one globally ordered log. A write succeeds only if a majority of nodes is alive;
// it is then delivered to every alive node's commit callback in log order. Killed nodes
// miss deliveries and catch up on Revive(). Followers answer NOT_LEADER with a hint.
// It deliberately does NOT model elections, terms or message loss: SetLeader() is how a
// test says "the election finished".
// ---------------------------------------------------------------------------
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lsmdb/consensus/raft_proposer.h"
#include "lsmdb/rpc/status.h"

namespace lsmdb::testing_support {

class FakeRaftCluster {
 public:
  FakeRaftCluster(std::vector<std::uint32_t> node_ids, std::uint32_t initial_leader)
      : leader_(initial_leader) {
    for (auto id : node_ids) nodes_[id] = Node{};
  }

  std::shared_ptr<consensus::RaftProposer> ProposerFor(std::uint32_t id) {
    return std::make_shared<View>(this, id);
  }

  void SetAddress(std::uint32_t id, v1::NodeInfo info) {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.at(id).address = std::move(info);
  }
  void SetLeader(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    leader_ = id;
  }
  void Kill(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.at(id).alive = false;
  }
  void Revive(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.at(id).alive = true;
    Deliver(nodes_.at(id));  // catch up on everything committed while it was down
  }
  std::size_t log_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return log_.size();
  }

 private:
  struct Node {
    bool alive = true;
    consensus::CommitCallback callback;
    std::uint64_t delivered = 0;
    v1::NodeInfo address;
  };

  class View final : public consensus::RaftProposer {
   public:
    View(FakeRaftCluster* c, std::uint32_t id) : cluster_(c), id_(id) {}
    consensus::ProposalResult Propose(const std::string& payload, std::chrono::milliseconds) override {
      return cluster_->ProposeFrom(id_, payload);
    }
    void SetCommitCallback(consensus::CommitCallback cb) override {
      std::lock_guard<std::mutex> lock(cluster_->mu_);
      Node& n = cluster_->nodes_.at(id_);
      n.callback = std::move(cb);
      if (n.alive) cluster_->Deliver(n);
    }

   private:
    FakeRaftCluster* cluster_;
    std::uint32_t id_;
  };

  void Deliver(Node& n) {  // caller holds mu_
    if (!n.callback) return;
    while (n.delivered < log_.size()) {
      ++n.delivered;
      n.callback(n.delivered, log_[n.delivered - 1]);
    }
  }

  consensus::ProposalResult ProposeFrom(std::uint32_t id, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mu_);
    consensus::ProposalResult r;
    if (!nodes_.at(id).alive) {
      r.status = rpc::MakeStatus(v1::STATUS_CODE_UNAVAILABLE, "node is down");
      return r;
    }
    if (id != leader_) {
      const Node& leader = nodes_.at(leader_);
      if (leader.alive) {
        r.status = rpc::MakeStatus(v1::STATUS_CODE_NOT_LEADER, "not the leader");
        *r.status.mutable_leader_hint() = leader.address;
      } else {
        r.status = rpc::MakeStatus(v1::STATUS_CODE_UNAVAILABLE, "no leader available");
      }
      return r;
    }
    std::size_t alive = 0;
    for (const auto& [nid, n] : nodes_) alive += n.alive;
    if (alive * 2 <= nodes_.size()) {
      r.status = rpc::MakeStatus(v1::STATUS_CODE_UNAVAILABLE, "no quorum");
      return r;
    }
    log_.push_back(payload);
    for (auto& [nid, n] : nodes_) if (n.alive) Deliver(n);
    r.status = rpc::MakeStatus(v1::STATUS_CODE_OK);
    r.log_index = log_.size();
    return r;
  }

  mutable std::mutex mu_;
  std::map<std::uint32_t, Node> nodes_;
  std::vector<std::string> log_;
  std::uint32_t leader_;
};

}  // namespace lsmdb::testing_support
