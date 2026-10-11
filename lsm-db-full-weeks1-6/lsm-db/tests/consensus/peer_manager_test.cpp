// Startup-order, failure-detection and reconnect behaviour with REAL nodes over gRPC.
#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include "lsmdb/consensus/peer_manager.h"
#include "../rpc/test_support.h"

using namespace lsmdb;
using namespace lsmdb::consensus;
using lsmdb::testing_support::MakeNodeInfo;
using lsmdb::testing_support::TestNode;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {
PeerManagerOptions FastOptions() {
  PeerManagerOptions o;
  o.ping_interval = milliseconds(100);
  o.ping_timeout = milliseconds(300);
  o.failures_to_down = 2;
  return o;
}

// Learns a currently-free port by starting and stopping a node on an ephemeral one.
std::uint32_t FreePort() {
  TestNode probe(99);
  return probe.port();
}
}  // namespace

TEST(PeerManager, StartsUpWhenPeersAreAlreadyRunning) {
  TestNode n2(2), n3(3);
  PeerManager pm(1, {MakeNodeInfo(2, "127.0.0.1", n2.port()), MakeNodeInfo(3, "127.0.0.1", n3.port())}, FastOptions());
  EXPECT_EQ(pm.StateOf(2), PeerState::kUnknown);
  pm.Start();
  EXPECT_TRUE(pm.WaitForAllUp(seconds(5)));
  EXPECT_EQ(pm.StateOf(2), PeerState::kUp);
  EXPECT_EQ(pm.StateOf(3), PeerState::kUp);
}

TEST(PeerManager, ToleratesAnyStartupOrder) {
  const std::uint32_t p2 = FreePort();
  std::uint32_t p3 = FreePort();
  while (p3 == p2) p3 = FreePort();
  PeerManager pm(1, {MakeNodeInfo(2, "127.0.0.1", p2), MakeNodeInfo(3, "127.0.0.1", p3)}, FastOptions());
  pm.Start();  // nobody is listening yet

  EXPECT_TRUE(pm.WaitForState(2, PeerState::kDown, seconds(5)));
  EXPECT_TRUE(pm.WaitForState(3, PeerState::kDown, seconds(5)));

  std::unique_ptr<TestNode> late3, late2;
  late3 = std::make_unique<TestNode>(3, std::vector<v1::NodeInfo>{}, nullptr, p3);  // node 3 first
  EXPECT_TRUE(pm.WaitForState(3, PeerState::kUp, seconds(10)));
  EXPECT_EQ(pm.StateOf(2), PeerState::kDown);                                        // 2 still missing
  late2 = std::make_unique<TestNode>(2, std::vector<v1::NodeInfo>{}, nullptr, p2);
  EXPECT_TRUE(pm.WaitForAllUp(seconds(10)));
}

TEST(PeerManager, DetectsPeerFailureAndRecovery) {
  TestNode n2(2);
  const std::uint32_t p3 = FreePort();
  auto n3 = std::make_unique<TestNode>(3, std::vector<v1::NodeInfo>{}, nullptr, p3);
  PeerManager pm(1, {MakeNodeInfo(2, "127.0.0.1", n2.port()), MakeNodeInfo(3, "127.0.0.1", p3)}, FastOptions());

  std::mutex mu;
  std::vector<std::tuple<std::uint32_t, PeerState, PeerState>> events;
  pm.SetStateCallback([&](std::uint32_t id, PeerState from, PeerState to) {
    std::lock_guard<std::mutex> lock(mu);
    events.emplace_back(id, from, to);
  });
  pm.Start();
  ASSERT_TRUE(pm.WaitForAllUp(seconds(5)));

  n3.reset();  // node 3 crashes
  EXPECT_TRUE(pm.WaitForState(3, PeerState::kDown, seconds(5)));
  EXPECT_EQ(pm.StateOf(2), PeerState::kUp);  // a healthy peer is unaffected

  n3 = std::make_unique<TestNode>(3, std::vector<v1::NodeInfo>{}, nullptr, p3);  // restarts at the same address
  EXPECT_TRUE(pm.WaitForState(3, PeerState::kUp, seconds(15)));

  std::lock_guard<std::mutex> lock(mu);
  bool saw_up_to_down = false, saw_down_to_up = false;
  for (auto& [id, from, to] : events) {
    if (id == 3 && from == PeerState::kUp && to == PeerState::kDown) saw_up_to_down = true;
    if (id == 3 && from == PeerState::kDown && to == PeerState::kUp) saw_down_to_up = true;
  }
  EXPECT_TRUE(saw_up_to_down);
  EXPECT_TRUE(saw_down_to_up);
}

TEST(PeerManager, RejectsAPeerThatAnswersWithTheWrongIdentity) {
  TestNode impostor(7);  // listens where we expect node 2, but identifies as node 7
  PeerManager pm(1, {MakeNodeInfo(2, "127.0.0.1", impostor.port())}, FastOptions());
  pm.Start();
  EXPECT_TRUE(pm.WaitForState(2, PeerState::kDown, seconds(5)));
  EXPECT_NE(pm.StateOf(2), PeerState::kUp);
}

TEST(PeerManager, ExposesChannelsAndIgnoresSelf) {
  TestNode n2(2);
  PeerManager pm(1, {MakeNodeInfo(1, "127.0.0.1", 1), MakeNodeInfo(2, "127.0.0.1", n2.port())}, FastOptions());
  EXPECT_EQ(pm.Snapshot().size(), 1u);          // self removed
  EXPECT_NE(pm.ChannelFor(2), nullptr);
  EXPECT_EQ(pm.ChannelFor(1), nullptr);
  EXPECT_EQ(pm.ChannelFor(42), nullptr);
  EXPECT_EQ(pm.StateOf(42), PeerState::kUnknown);
}

TEST(PeerManager, StopIsIdempotentAndDestructorJoins) {
  TestNode n2(2);
  auto pm = std::make_unique<PeerManager>(1, std::vector<v1::NodeInfo>{MakeNodeInfo(2, "127.0.0.1", n2.port())}, FastOptions());
  pm->Start();
  pm->Start();  // second Start is a no-op
  pm->Stop();
  pm->Stop();
  pm.reset();
}
