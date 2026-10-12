#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <random>
#include <chrono>
#include <memory>
#include <unordered_set>
#include "common/status.h"

namespace lsm {

enum class RaftRole {
    Follower = 0,
    Candidate = 1,
    Leader = 2
};

[[nodiscard]] std::string RaftRoleToString(RaftRole role);
std::ostream& operator<<(std::ostream& os, RaftRole role);

struct RequestVoteArgs {
    uint64_t term = 0;
    std::string candidate_id;
    uint64_t last_log_index = 0;
    uint64_t last_log_term = 0;
};

struct RequestVoteReply {
    uint64_t term = 0;
    bool vote_granted = false;
};

struct RaftPersistentState {
    uint64_t current_term = 0;
    std::string voted_for = "";

    Status SaveToFile(const std::string& file_path) const;
    Status LoadFromFile(const std::string& file_path);
};

/**
 * @brief Core Raft state machine handling terms, voting, and election transitions.
 *
 * Owned by Atishay as part of the Week 5 consensus foundation milestone.
 * Network RPC transport is handled via Vinayak's gRPC layer, and heartbeat timing
 * is coordinated with Sarthak's consensus timers.
 */
class RaftStateMachine {
public:
    RaftStateMachine(std::string node_id,
                     std::vector<std::string> peers,
                     std::string state_dir,
                     int min_timeout_ms = 150,
                     int max_timeout_ms = 300);
    ~RaftStateMachine() = default;

    // Initialization and recovery
    Status Init();

    // Inquiries
    [[nodiscard]] RaftRole Role() const noexcept { return role_; }
    [[nodiscard]] uint64_t CurrentTerm() const noexcept { return persistent_state_.current_term; }
    [[nodiscard]] const std::string& VotedFor() const noexcept { return persistent_state_.voted_for; }
    [[nodiscard]] const std::string& NodeId() const noexcept { return node_id_; }
    [[nodiscard]] bool IsLeader() const noexcept { return role_ == RaftRole::Leader; }

    // Core Election Transitions
    /**
     * @brief Transitions to Candidate, increments term, votes for self, and prepares RequestVote.
     */
    RequestVoteArgs StartElection();

    /**
     * @brief Processes an incoming RequestVote RPC from a peer candidate.
     */
    void HandleRequestVote(const RequestVoteArgs& args, RequestVoteReply* reply);

    /**
     * @brief Receives a vote reply from a peer; promotes to Leader if majority quorum reached.
     * @return true if this vote triggered promotion to Leader.
     */
    bool HandleVoteReply(const std::string& peer, const RequestVoteReply& reply);

    /**
     * @brief Reverts to Follower state upon discovering a higher term or valid leader.
     */
    void StepDown(uint64_t higher_term);

    /**
     * @brief Generates a randomized election timeout duration within [min_timeout, max_timeout].
     */
    [[nodiscard]] std::chrono::milliseconds GetRandomElectionTimeout();

    // Sets last log metadata for log up-to-date checks (§5.4)
    void SetLastLogMetadata(uint64_t last_index, uint64_t last_term);

private:
    std::string node_id_;
    std::vector<std::string> peers_;
    std::string state_dir_;
    int min_timeout_ms_;
    int max_timeout_ms_;

    RaftRole role_{RaftRole::Follower};
    RaftPersistentState persistent_state_;
    size_t votes_received_{0};
    std::unordered_set<std::string> voters_for_current_election_;

    uint64_t last_log_index_{0};
    uint64_t last_log_term_{0};

    std::mt19937 rng_;
    std::uniform_int_distribution<int> timeout_dist_;
};

} // namespace lsm
