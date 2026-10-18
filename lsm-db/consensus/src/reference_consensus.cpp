#include "lsmdb/consensus/reference_consensus.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace lsmdb::consensus {

ReferenceConsensus::ReferenceConsensus(std::shared_ptr<StateMachine> sm, ReferenceConsensusOptions o)
    : sm_(std::move(sm)), opt_(o) {
  if (!sm_) throw std::invalid_argument("ReferenceConsensus needs a state machine");
  commit_index_ = sm_->LastApplied();  // entries before that are already in storage
  // The log of a fresh stand-in starts empty; indexes continue after what storage already has.
  log_.assign(static_cast<std::size_t>(commit_index_), std::string());
}

ProposeResult ReferenceConsensus::Propose(const Command& command) {
  ProposeResult r;
  if (ValidationResult v = Validate(command); !v) {
    r.status = ProposeStatus::kInvalid;
    r.message = v.message;
    return r;
  }
  std::unique_lock<std::mutex> lock(mu_);
  if (stopped_) {
    r.status = ProposeStatus::kStopped;
    r.message = stop_reason_;
    return r;
  }
  if (!opt_.is_leader) {
    r.status = ProposeStatus::kNotLeader;
    r.leader_hint = opt_.leader_hint;
    r.message = "not the leader";
    return r;
  }
  log_.push_back(Encode(command));
  r.index = log_.size();
  r.status = ProposeStatus::kAccepted;
  if (opt_.auto_commit) CommitLocked(r.index, lock);
  if (stopped_) {  // apply failed for our entry (or an earlier one)
    r.status = ProposeStatus::kStopped;
    r.message = stop_reason_;
  }
  return r;
}

bool ReferenceConsensus::CommitLocked(LogIndex target, std::unique_lock<std::mutex>&) {
  target = std::min<LogIndex>(target, log_.size());
  while (!stopped_ && commit_index_ < target) {
    const LogIndex next = commit_index_ + 1;
    try {
      sm_->Apply(next, log_[next - 1]);
    } catch (const std::exception& e) {
      stopped_ = true;
      stop_reason_ = std::string("apply failed at index ") + std::to_string(next) + ": " + e.what();
      cv_.notify_all();
      return false;
    }
    commit_index_ = next;
  }
  cv_.notify_all();
  return !stopped_;
}

bool ReferenceConsensus::CommitUpTo(LogIndex index) {
  std::unique_lock<std::mutex> lock(mu_);
  return CommitLocked(index, lock);
}

bool ReferenceConsensus::CommitAll() { return CommitUpTo(std::numeric_limits<LogIndex>::max()); }

bool ReferenceConsensus::WaitApplied(LogIndex index, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait_for(lock, timeout, [&] { return commit_index_ >= index || stopped_; });
  return commit_index_ >= index;
}

ReadBarrierResult ReferenceConsensus::ReadBarrier(std::chrono::milliseconds timeout) {
  (void)timeout;  // all committed entries are applied synchronously by this single-node stand-in
  std::lock_guard<std::mutex> lock(mu_);
  ReadBarrierResult result;
  if (stopped_) {
    result.status = ReadBarrierStatus::kUnavailable;
    result.message = stop_reason_;
  } else if (!opt_.is_leader) {
    result.status = ReadBarrierStatus::kNotLeader;
    result.leader_hint = opt_.leader_hint;
    result.message = "not the leader";
  } else {
    result.status = ReadBarrierStatus::kReady;
    result.index = commit_index_;
  }
  return result;
}

void ReferenceConsensus::SetLeader(bool is_leader, std::uint32_t leader_hint) {
  std::lock_guard<std::mutex> lock(mu_);
  opt_.is_leader = is_leader;
  opt_.leader_hint = leader_hint;
}

void ReferenceConsensus::Stop() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!stopped_) {
    stopped_ = true;
    stop_reason_ = "stopped";
  }
  cv_.notify_all();
}

LogIndex ReferenceConsensus::LastIndex() const { std::lock_guard<std::mutex> l(mu_); return log_.size(); }
LogIndex ReferenceConsensus::CommitIndex() const { std::lock_guard<std::mutex> l(mu_); return commit_index_; }
bool ReferenceConsensus::stopped() const { std::lock_guard<std::mutex> l(mu_); return stopped_; }
std::string ReferenceConsensus::stop_reason() const { std::lock_guard<std::mutex> l(mu_); return stop_reason_; }
std::vector<std::string> ReferenceConsensus::LogSnapshot() const { std::lock_guard<std::mutex> l(mu_); return log_; }

}  // namespace lsmdb::consensus
