#include "lsmdb/consensus/peer_discovery.h"

#include <stdexcept>

namespace lsmdb::consensus {
namespace {
std::uint32_t ParseNumber(const std::string& text, const std::string& what) {
  if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos) {
    throw std::invalid_argument(what + " must be a number, got '" + text + "'");
  }
  return static_cast<std::uint32_t>(std::stoul(text));
}
}  // namespace

std::uint32_t PodOrdinal(const std::string& pod_name) {
  auto dash = pod_name.rfind('-');
  if (dash == std::string::npos || dash == 0 || dash + 1 >= pod_name.size()) {
    throw std::invalid_argument("pod name '" + pod_name + "' does not end in -<ordinal>");
  }
  return ParseNumber(pod_name.substr(dash + 1), "pod ordinal in '" + pod_name + "'");
}

std::string PodDnsName(const DiscoveryConfig& c, std::uint32_t ordinal) {
  std::string statefulset = c.pod_name.substr(0, c.pod_name.rfind('-'));
  return statefulset + "-" + std::to_string(ordinal) + "." + c.headless_service + "." +
         c.k8s_namespace + ".svc." + c.cluster_domain;
}

ClusterIdentity Discover(const DiscoveryConfig& c) {
  if (c.replicas == 0) throw std::invalid_argument("replicas must be >= 1");
  if (c.headless_service.empty()) throw std::invalid_argument("headless_service must not be empty");
  const std::uint32_t me = PodOrdinal(c.pod_name);
  if (me >= c.replicas) {
    throw std::invalid_argument("pod ordinal " + std::to_string(me) + " >= replicas " +
                                std::to_string(c.replicas));
  }
  ClusterIdentity id;
  for (std::uint32_t ordinal = 0; ordinal < c.replicas; ++ordinal) {
    v1::NodeInfo info;
    info.set_node_id(ordinal + 1);
    info.set_host(PodDnsName(c, ordinal));
    info.set_port(c.port);
    if (ordinal == me) id.self = info;
    else id.peers.push_back(info);
  }
  return id;
}

std::optional<DiscoveryConfig> DiscoveryConfigFromEnv(const EnvGetter& get) {
  auto value = [&](const char* name) -> std::string {
    const char* v = get(name);
    return v ? v : "";
  };
  DiscoveryConfig c;
  c.headless_service = value("LSMDB_HEADLESS_SERVICE");
  if (c.headless_service.empty()) return std::nullopt;

  c.pod_name = value("LSMDB_POD_NAME");
  if (c.pod_name.empty()) c.pod_name = value("HOSTNAME");
  if (c.pod_name.empty()) throw std::invalid_argument("discovery needs LSMDB_POD_NAME or HOSTNAME");
  if (auto v = value("LSMDB_NAMESPACE"); !v.empty()) c.k8s_namespace = v;
  if (auto v = value("LSMDB_CLUSTER_DOMAIN"); !v.empty()) c.cluster_domain = v;
  if (auto v = value("LSMDB_REPLICAS"); !v.empty()) c.replicas = ParseNumber(v, "LSMDB_REPLICAS");
  if (auto v = value("LSMDB_PORT"); !v.empty()) c.port = ParseNumber(v, "LSMDB_PORT");
  return c;
}

}  // namespace lsmdb::consensus
