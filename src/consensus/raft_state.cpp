#include "consensus/raft_state.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lsm {

std::string RaftRoleToString(RaftRole role) {
    switch (role) {
        case RaftRole::Follower: return "Follower";
        case RaftRole::Candidate: return "Candidate";
        case RaftRole::Leader: return "Leader";
        default: return "Unknown";
    }
}

std::ostream& operator<<(std::ostream& os, RaftRole role) {
    os << RaftRoleToString(role);
    return os;
}


// ---------------------------------------------------------------------------
// RaftPersistentState
// ---------------------------------------------------------------------------

Status RaftPersistentState::SaveToFile(const std::string& file_path) const {
    if (file_path.empty()) {
        return Status::Ok(); // In-memory testing
    }

    std::filesystem::path p(file_path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            return Status::IOError("Failed to create state directory: " + ec.message());
        }
    }

    const std::string temp_path = file_path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Status::IOError("Could not write Raft persistent state to: " + temp_path);
    }

    out.write(reinterpret_cast<const char*>(&current_term), sizeof(current_term));
    uint32_t len = static_cast<uint32_t>(voted_for.size());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (len > 0) {
        out.write(voted_for.data(), len);
    }

    out.flush();
    if (!out) {
        return Status::IOError("Failed during Raft state file write");
    }
    out.close();
#ifdef _WIN32
    if (!MoveFileExA(temp_path.c_str(), file_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
#else
    if (std::rename(temp_path.c_str(), file_path.c_str()) != 0)
#endif
    {
        return Status::IOError("Failed to atomically replace Raft persistent state file");
    }
    return Status::Ok();
}

Status RaftPersistentState::LoadFromFile(const std::string& file_path) {
    if (file_path.empty() || !std::filesystem::exists(file_path)) {
        current_term = 0;
        voted_for = "";
        return Status::Ok();
    }

    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        return Status::IOError("Could not read Raft persistent state from: " + file_path);
    }

    in.read(reinterpret_cast<char*>(&current_term), sizeof(current_term));
    if (!in) return Status::Corruption("Truncated Raft persistent state term");
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in || len > 1024 * 1024) return Status::Corruption("Invalid Raft persistent state vote length");
    if (len > 0) {
        voted_for.resize(len);
        in.read(voted_for.data(), len);
    } else {
        voted_for.clear();
    }

    if (!in) {
        return Status::Corruption("Failed to parse Raft persistent state file");
    }
    if (in.peek() != EOF) return Status::Corruption("Trailing bytes in Raft persistent state file");
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// RaftStateMachine
// ---------------------------------------------------------------------------

RaftStateMachine::RaftStateMachine(std::string node_id,
                                   std::vector<std::string> peers,
                                   std::string state_dir,
                                   int min_timeout_ms,
                                   int max_timeout_ms)
    : node_id_(std::move(node_id)),
      peers_(std::move(peers)),
      state_dir_(std::move(state_dir)),
      min_timeout_ms_(min_timeout_ms),
      max_timeout_ms_(max_timeout_ms),
      rng_(std::random_device{}()),
      timeout_dist_(min_timeout_ms_, max_timeout_ms_) {}

Status RaftStateMachine::Init() {
    if (!state_dir_.empty()) {
        std::string state_file = state_dir_ + "/raft_state.bin";
        Status s = persistent_state_.LoadFromFile(state_file);
        if (!s.ok()) return s;
    }
    role_ = RaftRole::Follower;
    votes_received_ = 0;
    return Status::Ok();
}

RequestVoteArgs RaftStateMachine::StartElection() {
    role_ = RaftRole::Candidate;
    persistent_state_.current_term++;
    persistent_state_.voted_for = node_id_;
    if (!state_dir_.empty()) {
        persistent_state_.SaveToFile(state_dir_ + "/raft_state.bin");
    }

    voters_for_current_election_.clear();
    voters_for_current_election_.insert(node_id_);
    votes_received_ = 1; // Voted for self

    RequestVoteArgs args;
    args.term = persistent_state_.current_term;
    args.candidate_id = node_id_;
    args.last_log_index = last_log_index_;
    args.last_log_term = last_log_term_;
    return args;
}

void RaftStateMachine::HandleRequestVote(const RequestVoteArgs& args, RequestVoteReply* reply) {
    if (reply == nullptr) return;

    reply->term = persistent_state_.current_term;
    reply->vote_granted = false;

    // Rule: Reject votes from candidates in smaller terms
    if (args.term < persistent_state_.current_term) {
        return;
    }

    // Rule: If term is higher, step down and update current term
    if (args.term > persistent_state_.current_term) {
        StepDown(args.term);
        reply->term = persistent_state_.current_term;
    }

    // Raft Election Safety (§5.4.1): Up-to-date log check
    bool log_is_up_to_date = false;
    if (args.last_log_term > last_log_term_) {
        log_is_up_to_date = true;
    } else if (args.last_log_term == last_log_term_ && args.last_log_index >= last_log_index_) {
        log_is_up_to_date = true;
    }

    // Grant vote if we haven't voted for someone else in this term and log is valid
    if ((persistent_state_.voted_for.empty() || persistent_state_.voted_for == args.candidate_id) &&
        log_is_up_to_date) {
        persistent_state_.voted_for = args.candidate_id;
        if (!state_dir_.empty()) {
            persistent_state_.SaveToFile(state_dir_ + "/raft_state.bin");
        }
        reply->vote_granted = true;
    }
}

bool RaftStateMachine::HandleVoteReply(const std::string& peer, const RequestVoteReply& reply) {
    (void)peer;
    if (role_ != RaftRole::Candidate) {
        return false;
    }

    if (reply.term > persistent_state_.current_term) {
        StepDown(reply.term);
        return false;
    }

    if (reply.term == persistent_state_.current_term && reply.vote_granted) {
        // A peer may retransmit a reply. Raft counts one vote per server.
        if (peer.empty() || peer == node_id_ ||
            std::find(peers_.begin(), peers_.end(), peer) == peers_.end() ||
            !voters_for_current_election_.insert(peer).second) {
            return false;
        }
        votes_received_++;
        std::unordered_set<std::string> unique_peers;
        for (const auto& id : peers_) {
            if (!id.empty() && id != node_id_) unique_peers.insert(id);
        }
        size_t cluster_size = unique_peers.size() + 1; // unique peers + self
        size_t quorum = (cluster_size / 2) + 1;

        if (votes_received_ >= quorum) {
            role_ = RaftRole::Leader;
            return true;
        }
    }

    return false;
}

void RaftStateMachine::StepDown(uint64_t higher_term) {
    role_ = RaftRole::Follower;
    votes_received_ = 0;

    if (higher_term > persistent_state_.current_term) {
        persistent_state_.current_term = higher_term;
        persistent_state_.voted_for = "";
        if (!state_dir_.empty()) {
            persistent_state_.SaveToFile(state_dir_ + "/raft_state.bin");
        }
    }
}

std::chrono::milliseconds RaftStateMachine::GetRandomElectionTimeout() {
    return std::chrono::milliseconds(timeout_dist_(rng_));
}

void RaftStateMachine::SetLastLogMetadata(uint64_t last_index, uint64_t last_term) {
    last_log_index_ = last_index;
    last_log_term_ = last_term;
}

} // namespace lsm
