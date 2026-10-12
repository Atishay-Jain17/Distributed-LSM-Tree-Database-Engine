#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::reads {

struct LeadershipInfo {
  bool is_leader = false;
  std::uint64_t current_term = 0;
  std::uint64_t commit_index = 0;
  std::optional<v1::NodeInfo> leader_hint;
};

// Interface for checking leadership state and quorum confirmation.
// Real Raft (Atishay / Sarthak) implements this; stand-ins implement it for testing.
class LeaderLeaseProvider {
 public:
  virtual ~LeaderLeaseProvider() = default;

  // Checks whether this node currently considers itself the leader.
  virtual LeadershipInfo GetLeadershipInfo() const = 0;

  // Verifies that a quorum still acknowledges this leader (e.g. heartbeat or lease check).
  // Returns false if quorum is lost or leadership changed.
  virtual bool ConfirmQuorum(std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) = 0;
};

// Simple in-memory lease provider for stand-ins and unit tests.
class MockLeaderLeaseProvider : public LeaderLeaseProvider {
 public:
  explicit MockLeaderLeaseProvider(bool is_leader = true, std::uint64_t term = 1,
                                   std::uint64_t commit_index = 0)
      : is_leader_(is_leader), term_(term), commit_index_(commit_index), quorum_ok_(true) {}

  void SetLeader(bool is_leader) { is_leader_ = is_leader; }
  void SetTerm(std::uint64_t term) { term_ = term; }
  void SetCommitIndex(std::uint64_t idx) { commit_index_ = idx; }
  void SetLeaderHint(v1::NodeInfo hint) { leader_hint_ = std::move(hint); }
  void SetQuorumOk(bool ok) { quorum_ok_ = ok; }

  LeadershipInfo GetLeadershipInfo() const override {
    return {is_leader_, term_, commit_index_, leader_hint_};
  }

  bool ConfirmQuorum(std::chrono::milliseconds) override {
    return is_leader_ && quorum_ok_;
  }

 private:
  bool is_leader_;
  std::uint64_t term_;
  std::uint64_t commit_index_;
  std::optional<v1::NodeInfo> leader_hint_;
  bool quorum_ok_;
};

}  // namespace lsmdb::reads
