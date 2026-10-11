#pragma once
// A client that knows ALL nodes and finds the leader by itself (Week 6 write routing).
//
//  * Writes: sent to the last known leader. NOT_LEADER with a hint -> jump straight to the
//    hinted node; NOT_LEADER without a hint or UNAVAILABLE -> try the next node.
//  * Reads: served by the first reachable node (local read; may lag the leader briefly).
// Thread-safe.
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lsmdb/rpc/clients.h"

namespace lsmdb::rpc {

struct ClusterClientOptions {
  ClientOptions client;                                  // per-call deadline etc.
  std::size_t max_redirects = 4;                         // extra attempts beyond one pass over all nodes
  std::chrono::milliseconds retry_backoff{20};           // pause before moving to another node
};

class ClusterClient {
 public:
  explicit ClusterClient(std::vector<std::string> targets, ClusterClientOptions options = {});

  v1::Status Put(const std::string& key, const std::string& value);
  v1::Status Delete(const std::string& key);
  KvClient::GetResult Get(const std::string& key);

  std::string leader_target() const;  // last node that accepted a write ("" if none yet)

 private:
  std::shared_ptr<KvClient> ClientFor(const std::string& target);
  std::string NextTarget(const std::string& current) const;
  template <typename Op>
  v1::Status Write(Op op);

  const ClusterClientOptions options_;
  mutable std::mutex mu_;
  std::vector<std::string> targets_;
  std::string leader_;
  std::map<std::string, std::shared_ptr<KvClient>> clients_;
};

}  // namespace lsmdb::rpc
