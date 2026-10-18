#include "lsmdb/consensus/raft_node.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "lsmdb/consensus/command.h"

namespace lsmdb::consensus {
namespace {
constexpr char kStateMagic[] = "LSMRAFT1";
constexpr std::uint32_t kStateVersion = 1;
constexpr std::size_t kStateHeaderBytes = 8 + 4 + 8 + 4 + 4 + 8 + 8 + 8 + 8;

void Put32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void Put64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
std::uint32_t Get32(const std::string& s, std::size_t p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + i])) << (i * 8);
  return v;
}
std::uint64_t Get64(const std::string& s, std::size_t p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[p + i])) << (i * 8);
  return v;
}
std::uint32_t Crc32(const char* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= static_cast<unsigned char>(data[i]);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
bool IsDataCommand(const std::string& bytes) {
  auto decoded = Decode(bytes);
  return decoded && Validate(*decoded.command) &&
         (decoded.command->type == CommandType::kPut || decoded.command->type == CommandType::kDelete);
}
void SyncAndReplace(const std::filesystem::path& temporary, const std::filesystem::path& target) {
#ifdef _WIN32
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot reopen Raft state for sync");
  const BOOL synced = FlushFileBuffers(file);
  const BOOL closed = CloseHandle(file);
  if (!synced) throw std::runtime_error("cannot sync Raft state file");
  if (!closed) throw std::runtime_error("cannot close Raft state file after sync");
  if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("cannot atomically replace Raft state file");
#else
  int fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot reopen Raft state for sync");
  const int result = ::fsync(fd);
  ::close(fd);
  if (result != 0) throw std::runtime_error("cannot sync Raft state file");
  if (::rename(temporary.c_str(), target.c_str()) != 0) throw std::runtime_error("cannot replace Raft state file");
  auto parent = target.parent_path();
  if (parent.empty()) parent = ".";
  int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) throw std::runtime_error("cannot open Raft state directory for sync");
  const int dir_sync = ::fsync(dir);
  const int dir_error = errno;
  const int dir_close = ::close(dir);
  if (dir_sync != 0) {
    errno = dir_error;
    throw std::runtime_error("cannot sync Raft state directory");
  }
  if (dir_close != 0) throw std::runtime_error("cannot close Raft state directory after sync");
#endif
}
}  // namespace

RaftNode::RaftNode(std::uint32_t node_id, std::vector<v1::NodeInfo> peers,
                   std::shared_ptr<StateMachine> state_machine, std::filesystem::path state_dir,
                   std::uint64_t durable_storage_writes, RaftNodeOptions options)
    : node_id_(node_id), peers_(std::move(peers)), state_machine_(std::move(state_machine)),
      state_file_(std::move(state_dir) / "raft.state"), options_(options),
      rng_(std::random_device{}()) {
  if (node_id_ == 0 || !state_machine_) throw std::invalid_argument("RaftNode requires a node id and state machine");
  if (options_.election_timeout_min.count() <= 0 ||
      options_.election_timeout_max < options_.election_timeout_min ||
      options_.heartbeat_interval.count() <= 0 || options_.rpc_timeout.count() <= 0 ||
      options_.proposal_timeout.count() <= 0)
    throw std::invalid_argument("invalid Raft timing options");
  for (const auto& peer : peers_) {
    if (peer.node_id() == 0 || peer.node_id() == node_id_ || peer.host().empty() || peer.port() == 0 ||
        stubs_.count(peer.node_id()))
      throw std::invalid_argument("invalid or duplicate Raft peer configuration");
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    auto channel = grpc::CreateCustomChannel(peer.host() + ":" + std::to_string(peer.port()),
                                             grpc::InsecureChannelCredentials(), args);
    stubs_.emplace(peer.node_id(), v1::RaftService::NewStub(channel));
  }
  std::filesystem::create_directories(state_file_.parent_path());
  LoadAndRecover(durable_storage_writes);
  std::lock_guard<std::mutex> lock(mu_);
  ResetElectionDeadlineLocked();
}

RaftNode::~RaftNode() { Stop(); }

void RaftNode::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return;
  stopping_ = false;
  running_ = true;
  ResetElectionDeadlineLocked();
  election_thread_ = std::thread([this] { ElectionLoop(); });
  heartbeat_thread_ = std::thread([this] { HeartbeatLoop(); });
}

void RaftNode::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
    running_ = false;
    leader_ = false;
  }
  cv_.notify_all();
  if (election_thread_.joinable()) election_thread_.join();
  if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

bool RaftNode::IsLeader() const { std::lock_guard<std::mutex> lock(mu_); return leader_; }
std::uint64_t RaftNode::CurrentTerm() const { std::lock_guard<std::mutex> lock(mu_); return term_; }
std::uint32_t RaftNode::LeaderId() const { std::lock_guard<std::mutex> lock(mu_); return leader_id_; }
std::uint64_t RaftNode::CommitIndex() const { std::lock_guard<std::mutex> lock(mu_); return commit_index_; }
std::uint64_t RaftNode::LastApplied() const { std::lock_guard<std::mutex> lock(mu_); return last_applied_; }

void RaftNode::ResetElectionDeadlineLocked() {
  std::uniform_int_distribution<std::int64_t> dist(options_.election_timeout_min.count(),
                                                   options_.election_timeout_max.count());
  election_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(dist(rng_));
}

std::uint64_t RaftNode::TermAtLocked(std::uint64_t index) const {
  return index == 0 || index > log_.size() ? 0 : log_[static_cast<std::size_t>(index - 1)].term;
}

void RaftNode::PersistLocked() const {
  std::string bytes(kStateMagic, sizeof(kStateMagic) - 1);
  Put32(bytes, kStateVersion);
  Put64(bytes, term_);
  Put32(bytes, voted_for_);
  Put32(bytes, leader_id_);
  Put64(bytes, commit_index_);
  Put64(bytes, last_applied_);
  Put64(bytes, applied_writes_);
  Put64(bytes, log_.size());
  for (const auto& entry : log_) {
    if (entry.command.size() > kMaxEncodedCommandBytes)
      throw std::length_error("Raft command exceeds the configured entry limit");
    Put64(bytes, entry.term);
    Put32(bytes, static_cast<std::uint32_t>(entry.command.size()));
    bytes.append(entry.command);
  }
  Put32(bytes, Crc32(bytes.data(), bytes.size()));
  const auto temporary = state_file_.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create Raft state file");
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) throw std::runtime_error("cannot write Raft state file");
  }
  SyncAndReplace(temporary, state_file_);
}

void RaftNode::LoadAndRecover(std::uint64_t durable_storage_writes) {
  if (!std::filesystem::exists(state_file_)) {
    if (durable_storage_writes != 0)
      throw std::runtime_error("storage contains data but Raft state is missing; refusing unsafe startup");
    state_machine_->RestoreLastApplied(0);
    return;
  }
  std::ifstream in(state_file_, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < kStateHeaderBytes + 4 || bytes.compare(0, 8, kStateMagic, 8) != 0)
    throw std::runtime_error("Raft state file has an invalid header");
  if (Crc32(bytes.data(), bytes.size() - 4) != Get32(bytes, bytes.size() - 4))
    throw std::runtime_error("Raft state file checksum mismatch");
  if (Get32(bytes, 8) != kStateVersion) throw std::runtime_error("unsupported Raft state file version");

  term_ = Get64(bytes, 12);
  voted_for_ = Get32(bytes, 20);
  leader_id_ = 0;  // leadership is volatile and must be re-established after restart
  commit_index_ = Get64(bytes, 28);
  const std::uint64_t persisted_applied = Get64(bytes, 36);
  applied_writes_ = Get64(bytes, 44);
  const std::uint64_t count = Get64(bytes, 52);
  std::size_t offset = kStateHeaderBytes;
  if (count > 10000000) throw std::runtime_error("Raft state log is unreasonably large");
  log_.reserve(static_cast<std::size_t>(count));
  std::uint64_t previous_term = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (offset + 12 > bytes.size() - 4) throw std::runtime_error("truncated Raft log entry");
    const auto entry_term = Get64(bytes, offset);
    const auto size = Get32(bytes, offset + 8);
    offset += 12;
    if (entry_term == 0 || entry_term < previous_term || size > kMaxEncodedCommandBytes ||
        size > bytes.size() - 4 - offset)
      throw std::runtime_error("invalid or truncated Raft log entry");
    log_.push_back({entry_term, bytes.substr(offset, size)});
    offset += size;
    previous_term = entry_term;
  }
  if (offset != bytes.size() - 4 || commit_index_ > log_.size() || persisted_applied > commit_index_)
    throw std::runtime_error("Raft state indices or file length are inconsistent");
  if (term_ < previous_term || (voted_for_ != 0 && voted_for_ != node_id_ && !stubs_.count(voted_for_)))
    throw std::runtime_error("Raft term or vote in state file is inconsistent");

  std::uint64_t writes = 0;
  std::uint64_t recovered_applied = 0;
  for (std::uint64_t index = 1; index <= commit_index_; ++index) {
    if (IsDataCommand(log_[static_cast<std::size_t>(index - 1)].command)) ++writes;
    if (writes <= durable_storage_writes) recovered_applied = index;
  }
  if (writes < durable_storage_writes)
    throw std::runtime_error("durable storage and committed Raft log disagree");
  if (recovered_applied == 0 && durable_storage_writes != 0)
    throw std::runtime_error("durable storage has writes absent from the committed Raft log");
  last_applied_ = recovered_applied;
  applied_writes_ = durable_storage_writes;
  state_machine_->RestoreLastApplied(last_applied_);
  {
    std::lock_guard<std::mutex> lock(mu_);
    ApplyCommittedLocked();
    PersistLocked();
    ResetElectionDeadlineLocked();
  }
}

void RaftNode::ApplyCommittedLocked() {
  while (last_applied_ < commit_index_) {
    const std::uint64_t index = last_applied_ + 1;
    const auto& command = log_[static_cast<std::size_t>(index - 1)].command;
    const bool data_command = IsDataCommand(command);
    state_machine_->Apply(index, command);
    last_applied_ = index;
    if (data_command) ++applied_writes_;
    PersistLocked();
    cv_.notify_all();
  }
}

bool RaftNode::AdvanceCommitLocked(std::uint64_t index) {
  if (index <= commit_index_ || index > log_.size()) return true;
  commit_index_ = index;
  PersistLocked();
  ApplyCommittedLocked();
  cv_.notify_all();
  return true;
}

bool RaftNode::HasCurrentTermCommitLocked() const {
  for (std::uint64_t i = commit_index_; i > 0; --i)
    if (TermAtLocked(i) == term_) return true;
  return false;
}

void RaftNode::ElectionLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stopping_) {
    if (leader_) {
      cv_.wait(lock, [&] { return stopping_ || !leader_; });
      continue;
    }
    const auto deadline = election_deadline_;
    if (cv_.wait_until(lock, deadline, [&] { return stopping_ || leader_ || election_deadline_ != deadline; }))
      continue;
    lock.unlock();
    try { StartElection(); }
    catch (const std::exception& e) {
      lock.lock();
      background_error_ = e.what();
      leader_ = false;
      stopping_ = true;
      running_ = false;
      cv_.notify_all();
      continue;
    }
    lock.lock();
  }
}

void RaftNode::StartElection() {
  std::uint64_t election_term, last_index, last_term;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ || leader_) return;
    leader_ = false;
    leader_id_ = 0;
    election_term = ++term_;
    voted_for_ = node_id_;
    last_index = LastIndexLocked();
    last_term = TermAtLocked(last_index);
    ResetElectionDeadlineLocked();
    PersistLocked();
  }
  cv_.notify_all();

  std::size_t votes = 1;
  const std::size_t quorum = (peers_.size() + 1) / 2 + 1;
  for (const auto& peer : peers_) {
    v1::RequestVoteRequest request;
    request.mutable_meta()->set_sender_node_id(node_id_);
    request.set_term(election_term);
    request.set_candidate_id(node_id_);
    request.set_last_log_index(last_index);
    request.set_last_log_term(last_term);
    v1::RequestVoteResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + options_.rpc_timeout);
    auto it = stubs_.find(peer.node_id());
    if (it == stubs_.end() || !it->second->RequestVote(&context, request, &response).ok()) continue;
    std::lock_guard<std::mutex> lock(mu_);
    if (response.term() > term_) {
      term_ = response.term();
      voted_for_ = 0;
      leader_ = false;
      leader_id_ = 0;
      ResetElectionDeadlineLocked();
      PersistLocked();
      cv_.notify_all();
      return;
    }
    if (term_ != election_term || leader_) return;
    if (response.vote_granted()) ++votes;
  }
  bool became_leader = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!stopping_ && term_ == election_term && votes >= quorum) {
      leader_ = true;
      leader_id_ = node_id_;
      for (const auto& peer : peers_) {
        next_index_[peer.node_id()] = LastIndexLocked() + 1;
        match_index_[peer.node_id()] = 0;
      }
      ResetElectionDeadlineLocked();
      PersistLocked();
      became_leader = true;
    } else {
      ResetElectionDeadlineLocked();
    }
  }
  cv_.notify_all();
  if (became_leader) {
    // A current-term no-op gives ReadBarrier the commit point required by Raft's
    // linearizability rule, even before the first client write in this term.
    try { (void)Propose(Command::Noop()); }
    catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(mu_);
      background_error_ = e.what();
      leader_ = false;
      leader_id_ = 0;
      PersistLocked();
      cv_.notify_all();
    }
  }
}

bool RaftNode::ReplicatePeer(std::uint32_t peer_id, std::uint64_t target, std::uint64_t term,
                             std::chrono::steady_clock::time_point deadline) {
  std::lock_guard<std::mutex> serial(replication_mu_);
  while (std::chrono::steady_clock::now() < deadline) {
    v1::AppendEntriesRequest request;
    std::uint64_t previous_index = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = stubs_.find(peer_id);
      if (it == stubs_.end() || !leader_ || term_ != term || stopping_) return false;
      const auto next = std::clamp(next_index_[peer_id], std::uint64_t{1}, LastIndexLocked() + 1);
      previous_index = next - 1;
      request.mutable_meta()->set_sender_node_id(node_id_);
      request.set_term(term_);
      request.set_leader_id(node_id_);
      request.set_prev_log_index(previous_index);
      request.set_prev_log_term(TermAtLocked(previous_index));
      request.set_leader_commit(commit_index_);
      if (next <= LastIndexLocked()) {
        auto* entry = request.add_entries();
        entry->set_term(log_[static_cast<std::size_t>(next - 1)].term);
        entry->set_command(log_[static_cast<std::size_t>(next - 1)].command);
      }
    }
    v1::AppendEntriesResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + options_.rpc_timeout);
    auto stub = stubs_.at(peer_id);
    if (!stub->AppendEntries(&context, request, &response).ok()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (response.term() > term_) {
      term_ = response.term();
      voted_for_ = 0;
      leader_ = false;
      leader_id_ = 0;
      ResetElectionDeadlineLocked();
      PersistLocked();
      cv_.notify_all();
      return false;
    }
    if (!leader_ || term_ != term) return false;
    if (response.success()) {
      const auto matched = std::max(response.match_index(), previous_index);
      match_index_[peer_id] = std::max(match_index_[peer_id], matched);
      next_index_[peer_id] = match_index_[peer_id] + 1;
      if (match_index_[peer_id] >= target) return true;
    } else {
      const auto candidate = response.conflict_index() == 0 ? next_index_[peer_id] - 1
                                                            : response.conflict_index();
      next_index_[peer_id] = std::max<std::uint64_t>(1, std::min(candidate, next_index_[peer_id] - 1));
    }
  }
  return false;
}

bool RaftNode::ReplicateRound(std::uint64_t target, std::uint64_t term,
                              std::chrono::steady_clock::time_point deadline) {
  std::size_t matched = 1;
  for (const auto& peer : peers_) {
    if (ReplicatePeer(peer.node_id(), target, term, deadline)) ++matched;
  }
  return matched >= (peers_.size() + 1) / 2 + 1;
}

ProposeResult RaftNode::Propose(const Command& command) {
  ProposeResult result;
  if (auto valid = Validate(command); !valid) {
    result.status = ProposeStatus::kInvalid;
    result.message = valid.message;
    return result;
  }
  std::lock_guard<std::mutex> proposal(proposal_mu_);
  std::uint64_t term, index;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) { result.status = ProposeStatus::kStopped; result.message = "Raft node is stopping"; return result; }
    if (!leader_) {
      result.status = ProposeStatus::kNotLeader;
      result.leader_hint = leader_id_;
      for (const auto& peer : peers_) {
        if (peer.node_id() == leader_id_) {
          result.leader_host = peer.host();
          result.leader_port = peer.port();
          break;
        }
      }
      result.message = "not the Raft leader";
      return result;
    }
    term = term_;
    log_.push_back({term, Encode(command)});
    index = log_.size();
    PersistLocked();
  }
  const auto deadline = std::chrono::steady_clock::now() + options_.proposal_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ReplicateRound(index, term, deadline)) {
      std::lock_guard<std::mutex> lock(mu_);
      if (!leader_ || term_ != term) {
        result.status = ProposeStatus::kNotLeader;
        result.leader_hint = leader_id_;
        return result;
      }
      try {
        AdvanceCommitLocked(index);
      } catch (const std::exception& e) {
        leader_ = false;
        result.status = ProposeStatus::kStopped;
        result.message = std::string("local apply failed: ") + e.what();
        return result;
      }
      result.status = ProposeStatus::kAccepted;
      result.index = index;
      // Best-effort commit notification; the write is committed durably on this quorum.
      cv_.notify_all();
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  result.status = ProposeStatus::kUnavailable;
  result.index = index;
  result.message = "write was not committed: a Raft majority did not acknowledge before timeout";
  return result;
}

bool RaftNode::WaitApplied(LogIndex index, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, timeout, [&] { return last_applied_ >= index || stopping_; }) && last_applied_ >= index;
}

ReadBarrierResult RaftNode::ReadBarrier(std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> proposal(proposal_mu_);
  ReadBarrierResult result;
  std::uint64_t term, committed;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) { result.status = ReadBarrierStatus::kUnavailable; result.message = "Raft node is stopping"; return result; }
    if (!leader_) {
      result.status = ReadBarrierStatus::kNotLeader;
      result.leader_hint = leader_id_;
      for (const auto& peer : peers_) {
        if (peer.node_id() == leader_id_) {
          result.leader_host = peer.host();
          result.leader_port = peer.port();
          break;
        }
      }
      result.message = "not the Raft leader";
      return result;
    }
    if (!HasCurrentTermCommitLocked()) {
      result.status = ReadBarrierStatus::kUnavailable;
      result.message = "leader has not committed an entry in its current term";
      return result;
    }
    term = term_;
    committed = commit_index_;
  }
  if (!ReplicateRound(committed, term, std::chrono::steady_clock::now() + timeout)) {
    result.status = ReadBarrierStatus::kUnavailable;
    result.message = "read barrier could not confirm leadership with a quorum";
    return result;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!leader_ || term_ != term) {
    result.status = ReadBarrierStatus::kNotLeader;
    result.leader_hint = leader_id_;
    for (const auto& peer : peers_) {
      if (peer.node_id() == leader_id_) {
        result.leader_host = peer.host();
        result.leader_port = peer.port();
        break;
      }
    }
    result.message = "leadership changed during read barrier";
    return result;
  }
  result.status = ReadBarrierStatus::kReady;
  result.index = commit_index_;
  return result;
}

void RaftNode::HeartbeatLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stopping_) {
    cv_.wait_for(lock, options_.heartbeat_interval, [&] { return stopping_; });
    if (stopping_) break;
    if (!leader_) continue;
    const auto term = term_;
    const auto target = LastIndexLocked();
    lock.unlock();
    bool quorum = false;
    try {
      quorum = ReplicateRound(target, term,
          std::chrono::steady_clock::now() + options_.rpc_timeout * 2);
    } catch (const std::exception& e) {
      lock.lock();
      background_error_ = e.what();
      leader_ = false;
      leader_id_ = 0;
      stopping_ = true;
      running_ = false;
      cv_.notify_all();
      break;
    }
    lock.lock();
    if (quorum && leader_ && term_ == term && target > commit_index_ && TermAtLocked(target) == term_) {
      try { AdvanceCommitLocked(target); }
      catch (...) { leader_ = false; leader_id_ = 0; PersistLocked(); }
    }
  }
}

grpc::Status RaftNode::RequestVote(grpc::ServerContext*, const v1::RequestVoteRequest* request,
                                  v1::RequestVoteResponse* response) {
  if (request->candidate_id() == 0 || request->meta().sender_node_id() != request->candidate_id() ||
      !stubs_.count(request->candidate_id()))
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "candidate is not a configured peer");
  try {
  std::lock_guard<std::mutex> lock(mu_);
  response->set_term(term_);
  if (request->term() < term_) {
    response->mutable_status()->set_code(v1::STATUS_CODE_OK);
    return grpc::Status::OK;
  }
  bool term_changed = false;
  if (request->term() > term_) {
    term_ = request->term();
    voted_for_ = 0;
    leader_ = false;
    leader_id_ = 0;
    term_changed = true;
  }
  const auto local_last = LastIndexLocked();
  const auto local_term = TermAtLocked(local_last);
  const bool up_to_date = request->last_log_term() > local_term ||
      (request->last_log_term() == local_term && request->last_log_index() >= local_last);
  const bool grant = up_to_date && (voted_for_ == 0 || voted_for_ == request->candidate_id());
  if (grant) {
    voted_for_ = request->candidate_id();
    leader_ = false;
    leader_id_ = 0;
    ResetElectionDeadlineLocked();
  }
  if (grant || term_changed) PersistLocked();
  response->set_term(term_);
  response->set_vote_granted(grant);
  response->mutable_status()->set_code(v1::STATUS_CODE_OK);
  cv_.notify_all();
  return grpc::Status::OK;
  } catch (const std::exception& e) {
    return grpc::Status(grpc::StatusCode::INTERNAL, std::string("RequestVote failed: ") + e.what());
  }
}

grpc::Status RaftNode::AppendEntries(grpc::ServerContext*, const v1::AppendEntriesRequest* request,
                                    v1::AppendEntriesResponse* response) {
  if (request->leader_id() == 0 || request->meta().sender_node_id() != request->leader_id() ||
      !stubs_.count(request->leader_id()))
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "leader is not a configured peer");
  // Validate the complete batch before changing term or log state. A malformed
  // command must never become committed and then be silently skipped by apply.
  std::uint64_t previous_entry_term = request->prev_log_term();
  for (const auto& incoming : request->entries()) {
    if (incoming.term() == 0 || incoming.term() > request->term() ||
        incoming.term() < previous_entry_term || incoming.command().size() > kMaxEncodedCommandBytes)
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid Raft log entry");
    const auto decoded = Decode(incoming.command());
    if (!decoded || !Validate(*decoded.command))
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid replicated command");
    previous_entry_term = incoming.term();
  }
  try {
  std::lock_guard<std::mutex> lock(mu_);
  response->mutable_status()->set_code(v1::STATUS_CODE_OK);
  response->set_term(term_);
  if (request->term() < term_) return grpc::Status::OK;
  bool persistent_changed = false;
  if (request->term() > term_) {
    term_ = request->term();
    voted_for_ = 0;
    persistent_changed = true;
  }
  leader_ = false;
  leader_id_ = request->leader_id();
  ResetElectionDeadlineLocked();
  if (request->prev_log_index() > LastIndexLocked()) {
    response->set_conflict_index(LastIndexLocked() + 1);
    response->set_term(term_);
    PersistLocked();
    cv_.notify_all();
    return grpc::Status::OK;
  }
  if (TermAtLocked(request->prev_log_index()) != request->prev_log_term()) {
    const auto conflict_term = TermAtLocked(request->prev_log_index());
    std::uint64_t first = request->prev_log_index();
    while (first > 1 && TermAtLocked(first - 1) == conflict_term) --first;
    response->set_conflict_index(first);
    response->set_term(term_);
    PersistLocked();
    cv_.notify_all();
    return grpc::Status::OK;
  }
  std::uint64_t index = request->prev_log_index() + 1;
  bool changed = false;
  for (const auto& incoming : request->entries()) {
    if (index <= log_.size()) {
      const auto& existing = log_[static_cast<std::size_t>(index - 1)];
      if (existing.term == incoming.term() && existing.command == incoming.command()) { ++index; continue; }
      if (index <= commit_index_) return grpc::Status(grpc::StatusCode::DATA_LOSS, "AppendEntries conflicts with a committed entry");
      log_.resize(static_cast<std::size_t>(index - 1));
    }
    log_.push_back({incoming.term(), incoming.command()});
    changed = true;
    ++index;
  }
  if (request->leader_commit() > commit_index_) {
    commit_index_ = std::min(request->leader_commit(), LastIndexLocked());
    changed = true;
  }
  try { ApplyCommittedLocked(); }
  catch (const std::exception& e) {
    PersistLocked();
    return grpc::Status(grpc::StatusCode::INTERNAL, std::string("state-machine apply failed: ") + e.what());
  }
  if (changed || persistent_changed) PersistLocked();
  response->set_term(term_);
  response->set_success(true);
  response->set_match_index(request->prev_log_index() + request->entries_size());
  cv_.notify_all();
  return grpc::Status::OK;
  } catch (const std::exception& e) {
    return grpc::Status(grpc::StatusCode::INTERNAL, std::string("AppendEntries failed: ") + e.what());
  }
}

}  // namespace lsmdb::consensus
