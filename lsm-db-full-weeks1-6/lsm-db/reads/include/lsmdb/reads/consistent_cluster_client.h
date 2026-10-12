#pragma once
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lsmdb/rpc/clients.h"
#include "lsmdb/rpc/cluster_client.h"

namespace lsmdb::reads {

// High-level client that routes BOTH reads and writes to the cluster leader,
// following NOT_LEADER hints and recovering from leader crashes.
class ConsistentClusterClient {
 public:
  explicit ConsistentClusterClient(std::vector<std::string> targets,
                                   rpc::ClusterClientOptions options = {});

  v1::Status Put(const std::string& key, const std::string& value);
  v1::Status Delete(const std::string& key);
  rpc::KvClient::GetResult Get(const std::string& key);

  std::string leader_target() const;

 private:
  std::shared_ptr<rpc::KvClient> ClientFor(const std::string& target);
  std::string NextTarget(const std::string& current) const;

  const rpc::ClusterClientOptions options_;
  mutable std::mutex mu_;
  std::vector<std::string> targets_;
  std::string leader_;
  std::map<std::string, std::shared_ptr<rpc::KvClient>> clients_;
};

}  // namespace lsmdb::reads
