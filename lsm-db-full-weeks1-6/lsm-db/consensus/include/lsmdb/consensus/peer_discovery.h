#pragma once
// ---------------------------------------------------------------------------
// Kubernetes peer discovery (Week 5).
//
// A StatefulSet named "lsmdb" with 3 replicas creates pods lsmdb-0, lsmdb-1, lsmdb-2.
// Each pod has a stable DNS name through the headless Service:
//     <pod>.<service>.<namespace>.svc.<cluster-domain>
//     e.g. lsmdb-1.lsmdb.default.svc.cluster.local
//
// IDENTITY RULE (agreed contract with Suhani's Helm chart):
//     node_id = pod ordinal + 1        (lsmdb-0 -> 1, lsmdb-1 -> 2, lsmdb-2 -> 3)
// so Raft node ids are stable across restarts and never 0.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::consensus {

struct DiscoveryConfig {
  std::string pod_name;           // "lsmdb-1"
  std::string headless_service;   // "lsmdb"
  std::string k8s_namespace = "default";
  std::string cluster_domain = "cluster.local";
  std::uint32_t replicas = 3;
  std::uint32_t port = 50051;
};

struct ClusterIdentity {
  v1::NodeInfo self;
  std::vector<v1::NodeInfo> peers;  // every other replica, ordered by node_id
};

// "lsmdb-2" -> 2. Throws std::invalid_argument if the name has no "-<number>" suffix.
std::uint32_t PodOrdinal(const std::string& pod_name);

// "lsmdb-1.lsmdb.default.svc.cluster.local"
std::string PodDnsName(const DiscoveryConfig& config, std::uint32_t ordinal);

// Throws std::invalid_argument for a bad pod name, ordinal >= replicas, or replicas == 0.
ClusterIdentity Discover(const DiscoveryConfig& config);

using EnvGetter = std::function<const char*(const char*)>;

// Reads the discovery settings from environment variables:
//   LSMDB_HEADLESS_SERVICE  (required; its presence switches discovery ON)
//   LSMDB_POD_NAME          (falls back to HOSTNAME, which Kubernetes sets to the pod name)
//   LSMDB_NAMESPACE         (default "default")      LSMDB_CLUSTER_DOMAIN (default cluster.local)
//   LSMDB_REPLICAS          (default 3)              LSMDB_PORT           (default 50051)
// Returns nullopt when LSMDB_HEADLESS_SERVICE is unset. Throws std::invalid_argument for
// unparsable numbers or a missing pod name.
std::optional<DiscoveryConfig> DiscoveryConfigFromEnv(const EnvGetter& getenv_fn);

}  // namespace lsmdb::consensus
