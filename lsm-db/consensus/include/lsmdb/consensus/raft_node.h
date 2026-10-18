#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "lsmdb/consensus/boundary.h"
#include "lsmdb/v1/raft.grpc.pb.h"

namespace lsmdb::consensus {

struct RaftNodeOptions {
  std::chrono::milliseconds election_timeout_min{450};
  std::chrono::milliseconds election_timeout_max{800};
  std::chrono::milliseconds heartbeat_interval{100};
  std::chrono::milliseconds rpc_timeout{150};
  std::chrono::milliseconds proposal_timeout{3000};
};

// A compact, persistent Raft implementation used by the CLI node. It persists
// term/vote/log/commit state, requires a majority for writes and read barriers,
// and applies committed commands in order to the supplied state machine.
class RaftNode final : public Proposer, public v1::RaftService::Service {
 public:
  RaftNode(std::uint32_t node_id, std::vector<v1::NodeInfo> peers,
           std::shared_ptr<StateMachine> state_machine, std::filesystem::path state_dir,
           std::uint64_t durable_storage_writes, RaftNodeOptions options = {});
  ~RaftNode() override;
  RaftNode(const RaftNode&) = delete;
  RaftNode& operator=(const RaftNode&) = delete;

  void Start();
  void Stop();
  bool IsLeader() const;
  std::uint64_t CurrentTerm() const;
  std::uint32_t LeaderId() const;
  std::uint64_t CommitIndex() const;
  std::uint64_t LastApplied() const;

  ProposeResult Propose(const Command& command) override;
  bool WaitApplied(LogIndex index, std::chrono::milliseconds timeout) override;
  ReadBarrierResult ReadBarrier(std::chrono::milliseconds timeout) override;

  grpc::Status RequestVote(grpc::ServerContext*, const v1::RequestVoteRequest*,
                           v1::RequestVoteResponse*) override;
  grpc::Status AppendEntries(grpc::ServerContext*, const v1::AppendEntriesRequest*,
                            v1::AppendEntriesResponse*) override;

 private:
  struct Entry { std::uint64_t term; std::string command; };

  void ElectionLoop();
  void StartElection();
  void BecomeLeader(std::uint64_t term);
  void HeartbeatLoop();
  bool ReplicatePeer(std::uint32_t peer_id, std::uint64_t target, std::uint64_t term,
                     std::chrono::steady_clock::time_point deadline);
  bool ReplicateRound(std::uint64_t target, std::uint64_t term,
                     std::chrono::steady_clock::time_point deadline);
  bool HasCurrentTermCommitLocked() const;
  bool AdvanceCommitLocked(std::uint64_t index);
  void ApplyCommittedLocked();
  void PersistLocked() const;
  void LoadAndRecover(std::uint64_t durable_storage_writes);
  void ResetElectionDeadlineLocked();
  std::uint64_t LastIndexLocked() const { return log_.size(); }
  std::uint64_t TermAtLocked(std::uint64_t index) const;

  const std::uint32_t node_id_;
  const std::vector<v1::NodeInfo> peers_;
  const std::shared_ptr<StateMachine> state_machine_;
  const std::filesystem::path state_file_;
  const RaftNodeOptions options_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::mutex proposal_mu_;
  std::mutex replication_mu_;
  std::map<std::uint32_t, std::shared_ptr<v1::RaftService::Stub>> stubs_;
  std::map<std::uint32_t, std::uint64_t> next_index_;
  std::map<std::uint32_t, std::uint64_t> match_index_;
  std::vector<Entry> log_;
  std::uint64_t term_ = 0;
  std::uint32_t voted_for_ = 0;
  std::uint32_t leader_id_ = 0;
  std::uint64_t commit_index_ = 0;
  std::uint64_t last_applied_ = 0;
  std::uint64_t applied_writes_ = 0;
  std::chrono::steady_clock::time_point election_deadline_;
  std::mt19937_64 rng_;
  bool leader_ = false;
  bool running_ = false;
  bool stopping_ = false;
  std::string background_error_;
  std::thread election_thread_;
  std::thread heartbeat_thread_;
};

}  // namespace lsmdb::consensus
