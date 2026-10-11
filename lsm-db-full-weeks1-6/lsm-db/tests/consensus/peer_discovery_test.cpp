#include <gtest/gtest.h>

#include <map>
#include <stdexcept>

#include "lsmdb/consensus/peer_discovery.h"

using namespace lsmdb;
using namespace lsmdb::consensus;

namespace {
EnvGetter FakeEnv(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](const char* name) -> const char* {
    auto it = vars.find(name);
    return it == vars.end() ? nullptr : it->second.c_str();
  };
}
}  // namespace

TEST(PodOrdinal, ParsesStatefulSetNames) {
  EXPECT_EQ(PodOrdinal("lsmdb-0"), 0u);
  EXPECT_EQ(PodOrdinal("lsmdb-2"), 2u);
  EXPECT_EQ(PodOrdinal("my-lsm-db-12"), 12u);  // dashes in the set name are fine
}

TEST(PodOrdinal, RejectsNamesWithoutOrdinal) {
  for (const char* bad : {"", "lsmdb", "lsmdb-", "-3", "lsmdb-x", "lsmdb-1a"}) {
    EXPECT_THROW(PodOrdinal(bad), std::invalid_argument) << "name: " << bad;
  }
}

TEST(Discover, BuildsIdentityAndPeersFromPodName) {
  DiscoveryConfig c;
  c.pod_name = "lsmdb-1";
  c.headless_service = "lsmdb";
  c.k8s_namespace = "prod";
  auto id = Discover(c);

  EXPECT_EQ(id.self.node_id(), 2u);  // ordinal 1 -> node id 2
  EXPECT_EQ(id.self.host(), "lsmdb-1.lsmdb.prod.svc.cluster.local");
  EXPECT_EQ(id.self.port(), 50051u);
  ASSERT_EQ(id.peers.size(), 2u);
  EXPECT_EQ(id.peers[0].node_id(), 1u);
  EXPECT_EQ(id.peers[0].host(), "lsmdb-0.lsmdb.prod.svc.cluster.local");
  EXPECT_EQ(id.peers[1].node_id(), 3u);
  EXPECT_EQ(id.peers[1].host(), "lsmdb-2.lsmdb.prod.svc.cluster.local");
}

TEST(Discover, EveryPodSeesTheSameClusterFromItsOwnAngle) {
  for (std::uint32_t me = 0; me < 3; ++me) {
    DiscoveryConfig c;
    c.pod_name = "lsmdb-" + std::to_string(me);
    c.headless_service = "lsmdb";
    auto id = Discover(c);
    std::set<std::uint32_t> ids{id.self.node_id()};
    for (const auto& p : id.peers) ids.insert(p.node_id());
    EXPECT_EQ(ids, (std::set<std::uint32_t>{1, 2, 3}));
    EXPECT_EQ(id.self.node_id(), me + 1);
  }
}

TEST(Discover, RejectsBadConfiguration) {
  DiscoveryConfig c;
  c.headless_service = "lsmdb";
  c.pod_name = "lsmdb-3";  // ordinal 3 but only 3 replicas (0..2)
  EXPECT_THROW(Discover(c), std::invalid_argument);
  c.pod_name = "lsmdb-0";
  c.replicas = 0;
  EXPECT_THROW(Discover(c), std::invalid_argument);
  c.replicas = 3;
  c.headless_service = "";
  EXPECT_THROW(Discover(c), std::invalid_argument);
  c.headless_service = "lsmdb";
  c.pod_name = "noordinal";
  EXPECT_THROW(Discover(c), std::invalid_argument);
}

TEST(DiscoveryEnv, DisabledWithoutHeadlessService) {
  EXPECT_FALSE(DiscoveryConfigFromEnv(FakeEnv({{"HOSTNAME", "lsmdb-0"}})).has_value());
}

TEST(DiscoveryEnv, ReadsAllSettingsAndPrefersExplicitPodName) {
  auto c = DiscoveryConfigFromEnv(FakeEnv({{"LSMDB_HEADLESS_SERVICE", "svc"},
                                           {"LSMDB_POD_NAME", "db-2"},
                                           {"HOSTNAME", "ignored-9"},
                                           {"LSMDB_NAMESPACE", "team"},
                                           {"LSMDB_CLUSTER_DOMAIN", "corp.local"},
                                           {"LSMDB_REPLICAS", "5"},
                                           {"LSMDB_PORT", "6000"}}));
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->pod_name, "db-2");
  EXPECT_EQ(c->headless_service, "svc");
  EXPECT_EQ(c->k8s_namespace, "team");
  EXPECT_EQ(c->cluster_domain, "corp.local");
  EXPECT_EQ(c->replicas, 5u);
  EXPECT_EQ(c->port, 6000u);
}

TEST(DiscoveryEnv, FallsBackToHostnameAndDefaults) {
  auto c = DiscoveryConfigFromEnv(FakeEnv({{"LSMDB_HEADLESS_SERVICE", "lsmdb"}, {"HOSTNAME", "lsmdb-1"}}));
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->pod_name, "lsmdb-1");
  EXPECT_EQ(c->k8s_namespace, "default");
  EXPECT_EQ(c->replicas, 3u);
  EXPECT_EQ(c->port, 50051u);
}

TEST(DiscoveryEnv, ReportsUnusableValues) {
  EXPECT_THROW(DiscoveryConfigFromEnv(FakeEnv({{"LSMDB_HEADLESS_SERVICE", "lsmdb"}})), std::invalid_argument);  // no pod name
  EXPECT_THROW(DiscoveryConfigFromEnv(FakeEnv({{"LSMDB_HEADLESS_SERVICE", "lsmdb"},
                                               {"HOSTNAME", "lsmdb-0"},
                                               {"LSMDB_REPLICAS", "three"}})),
               std::invalid_argument);
}
