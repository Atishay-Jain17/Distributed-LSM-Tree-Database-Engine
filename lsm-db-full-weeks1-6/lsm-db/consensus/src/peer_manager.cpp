#include "lsmdb/consensus/peer_manager.h"

#include "lsmdb/rpc/clients.h"
#include "lsmdb/rpc/status.h"

namespace lsmdb::consensus {

const char* PeerStateName(PeerState s) {
  switch (s) {
    case PeerState::kUnknown: return "UNKNOWN";
    case PeerState::kUp: return "UP";
    case PeerState::kDown: return "DOWN";
  }
  return "?";
}

namespace {
std::shared_ptr<grpc::Channel> MakePeerChannel(const v1::NodeInfo& n) {
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 200);
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 200);
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
  args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, 1000);  // pods get new IPs on restart
  return grpc::CreateCustomChannel(n.host() + ":" + std::to_string(n.port()),
                                   grpc::InsecureChannelCredentials(), args);
}
}  // namespace

PeerManager::PeerManager(std::uint32_t self_id, std::vector<v1::NodeInfo> peers, PeerManagerOptions options)
    : self_id_(self_id), options_(options) {
  for (auto& p : peers) {
    if (p.node_id() == self_id_) continue;  // never connect to ourselves
    Peer peer;
    peer.status.info = p;
    peer.channel = MakePeerChannel(p);
    peers_.emplace(p.node_id(), std::move(peer));
  }
}

PeerManager::~PeerManager() { Stop(); }

void PeerManager::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
}

void PeerManager::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PeerManager::Loop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stop_) {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, peer] : peers_) ids.push_back(id);
    lock.unlock();
    for (auto id : ids) PingOne(id);
    lock.lock();
    cv_.wait_for(lock, options_.ping_interval, [&] { return stop_; });
  }
}

void PeerManager::PingOne(std::uint32_t peer_id) {
  std::shared_ptr<grpc::Channel> channel;
  {
    std::lock_guard<std::mutex> lock(mu_);
    channel = peers_.at(peer_id).channel;
  }
  rpc::ClientOptions co;
  co.deadline = options_.ping_timeout;
  co.sender_node_id = self_id_;
  auto result = rpc::NodeClient(channel, co).Ping();
  const bool ok = rpc::IsOk(result.status) && result.responder.node_id() == peer_id;

  PeerState from, to;
  {
    std::lock_guard<std::mutex> lock(mu_);
    Peer& p = peers_.at(peer_id);
    from = p.status.state;
    if (ok) {
      p.status.consecutive_failures = 0;
      p.status.state = PeerState::kUp;
    } else if (++p.status.consecutive_failures >= options_.failures_to_down) {
      p.status.state = PeerState::kDown;
    }
    to = p.status.state;
  }
  if (from != to) {
    cv_.notify_all();
    if (callback_) callback_(peer_id, from, to);
  }
}

PeerState PeerManager::StateOf(std::uint32_t peer_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(peer_id);
  return it == peers_.end() ? PeerState::kUnknown : it->second.status.state;
}

std::vector<PeerStatus> PeerManager::Snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PeerStatus> out;
  for (const auto& [id, peer] : peers_) out.push_back(peer.status);
  return out;
}

std::shared_ptr<grpc::Channel> PeerManager::ChannelFor(std::uint32_t peer_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(peer_id);
  return it == peers_.end() ? nullptr : it->second.channel;
}

bool PeerManager::WaitForState(std::uint32_t peer_id, PeerState state, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, timeout, [&] {
    auto it = peers_.find(peer_id);
    return it != peers_.end() && it->second.status.state == state;
  });
}

bool PeerManager::WaitForAllUp(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, timeout, [&] {
    for (const auto& [id, peer] : peers_) if (peer.status.state != PeerState::kUp) return false;
    return true;
  });
}

}  // namespace lsmdb::consensus
