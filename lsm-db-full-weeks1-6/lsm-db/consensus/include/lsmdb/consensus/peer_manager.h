#pragma once
// ---------------------------------------------------------------------------
// PeerManager (Week 5): keeps a gRPC connection to every peer and tracks whether
// each one is reachable.
//
//  * Startup order does not matter: peers that are not up yet are simply Down/Unknown
//    and are retried forever.
//  * A peer is declared Down after `failures_to_down` consecutive failed pings, and Up
//    again on the first success, so a restart is noticed automatically.
//  * Channels are tuned to reconnect quickly (200ms-1s backoff, DNS re-resolution every
//    1s) instead of gRPC's default of up to 2 minutes.
//
// Consumers: the Raft code (Weeks 5-6) takes a peer's channel via ChannelFor() to send
// RequestVote/AppendEntries, and reads StateOf() / the callback to know who is reachable.
// ---------------------------------------------------------------------------
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::consensus {

enum class PeerState { kUnknown, kUp, kDown };
const char* PeerStateName(PeerState s);

struct PeerStatus {
  v1::NodeInfo info;
  PeerState state = PeerState::kUnknown;
  std::uint32_t consecutive_failures = 0;
};

struct PeerManagerOptions {
  std::chrono::milliseconds ping_interval{500};
  std::chrono::milliseconds ping_timeout{300};
  std::uint32_t failures_to_down = 2;
};

class PeerManager {
 public:
  using StateCallback = std::function<void(std::uint32_t peer_id, PeerState from, PeerState to)>;

  PeerManager(std::uint32_t self_id, std::vector<v1::NodeInfo> peers, PeerManagerOptions options = {});
  ~PeerManager();
  PeerManager(const PeerManager&) = delete;
  PeerManager& operator=(const PeerManager&) = delete;

  // Must be set before Start(). Called from the monitor thread, never under an internal lock.
  void SetStateCallback(StateCallback cb) { callback_ = std::move(cb); }

  void Start();
  void Stop();  // idempotent

  PeerState StateOf(std::uint32_t peer_id) const;
  std::vector<PeerStatus> Snapshot() const;
  std::shared_ptr<grpc::Channel> ChannelFor(std::uint32_t peer_id) const;  // null if unknown id

  bool WaitForState(std::uint32_t peer_id, PeerState state, std::chrono::milliseconds timeout) const;
  bool WaitForAllUp(std::chrono::milliseconds timeout) const;

 private:
  struct Peer {
    PeerStatus status;
    std::shared_ptr<grpc::Channel> channel;
  };
  void Loop();
  void PingOne(std::uint32_t peer_id);

  const std::uint32_t self_id_;
  const PeerManagerOptions options_;
  StateCallback callback_;

  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  std::map<std::uint32_t, Peer> peers_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace lsmdb::consensus
