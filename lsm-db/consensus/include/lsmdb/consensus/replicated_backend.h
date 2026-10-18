#pragma once
// The KV layer's view of consensus: writes replicate and reads pass a quorum barrier.
//
//   Put/Delete -> Propose(Command) -> wait until applied locally -> return
//   Get        -> quorum read barrier -> wait for apply -> read local storage
//
// Drop-in for node_main.cpp: pass it to NodeServer instead of the plain storage backend.
#include <chrono>
#include <memory>
#include <stdexcept>

#include "lsmdb/consensus/boundary.h"
#include "lsmdb/rpc/storage_backend.h"

namespace lsmdb::consensus {

// Thrown by a node that is not the leader. The RPC layer reports the node ID and endpoint hint.
struct NotLeaderError : std::runtime_error {
  explicit NotLeaderError(std::uint32_t hint, std::string host = {}, std::uint32_t port = 0)
      : std::runtime_error("not the leader"), leader_hint(hint), leader_host(std::move(host)), leader_port(port) {}
  std::uint32_t leader_hint;  // 0 = unknown
  std::string leader_host;
  std::uint32_t leader_port;
};

struct ConsensusUnavailableError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class ReplicatedBackend : public rpc::StorageBackend {
 public:
  // `reader` is the same storage the StorageStateMachine writes to.
  ReplicatedBackend(std::shared_ptr<Proposer> proposer, std::shared_ptr<rpc::StorageBackend> reader,
                    std::chrono::milliseconds apply_timeout = std::chrono::seconds(5));

  void Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  void Delete(const std::string& key) override;

 private:
  void Replicate(const Command& command);

  std::shared_ptr<Proposer> proposer_;
  std::shared_ptr<rpc::StorageBackend> reader_;
  std::chrono::milliseconds timeout_;
};

}  // namespace lsmdb::consensus
