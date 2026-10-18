#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/consensus/raft_node.h"
#include "lsmdb/consensus/replicated_backend.h"
#include "lsmdb/consensus/storage_state_machine.h"
#include "lsmdb/node/persistent_backend.h"
#include "lsmdb/rpc/node_server.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
std::uint32_t ReservePort() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket failed while reserving test port");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    throw std::runtime_error("bind failed while reserving test port");
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    ::close(fd);
    throw std::runtime_error("getsockname failed while reserving test port");
  }
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

struct Node {
  std::shared_ptr<lsmdb::node::PersistentBackend> persistent;
  std::shared_ptr<lsmdb::rpc::StorageBackend> storage;
  std::shared_ptr<lsmdb::consensus::StorageStateMachine> state_machine;
  std::shared_ptr<lsmdb::consensus::RaftNode> raft;
  std::shared_ptr<lsmdb::consensus::ReplicatedBackend> backend;
  std::unique_ptr<lsmdb::rpc::NodeServer> server;
};

struct Cleanup {
  std::array<Node, 3>* nodes;
  fs::path root;
  ~Cleanup() {
    for (auto& node : *nodes) if (node.server) node.server->Shutdown();
    for (auto& node : *nodes) if (node.raft) node.raft->Stop();
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

bool WaitForLeader(const std::array<Node, 3>& nodes, int excluded = -1) {
  const auto end = std::chrono::steady_clock::now() + 8s;
  while (std::chrono::steady_clock::now() < end) {
    int leaders = 0;
    for (int i = 0; i < 3; ++i) if (i != excluded && nodes[i].raft->IsLeader()) ++leaders;
    if (leaders == 1) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

bool WaitForReplicatedValue(const std::array<Node, 3>& nodes, const std::string& value, int excluded = -1) {
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < end) {
    bool ready = true;
    for (int i = 0; i < 3; ++i) if (i != excluded) {
      ready = ready && nodes[i].storage->Get("cluster-key") == std::optional<std::string>(value) &&
              nodes[i].raft->CommitIndex() == nodes[i].raft->LastApplied();
    }
    if (ready) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

std::vector<lsmdb::v1::NodeInfo> PeersFor(int index, const std::array<std::uint32_t, 3>& ports) {
  std::vector<lsmdb::v1::NodeInfo> peers;
  for (int peer_index = 0; peer_index < 3; ++peer_index) if (peer_index != index) {
    lsmdb::v1::NodeInfo peer;
    peer.set_node_id(peer_index + 1);
    peer.set_host("127.0.0.1");
    peer.set_port(ports[peer_index]);
    peers.push_back(std::move(peer));
  }
  return peers;
}

void StartNode(Node& node, int index, const fs::path& root,
               const std::array<std::uint32_t, 3>& ports) {
  const auto peers = PeersFor(index, ports);
  node.persistent = lsmdb::node::PersistentBackend::Open(root / "data" / std::to_string(index + 1));
  node.storage = node.persistent;
  node.state_machine = std::make_shared<lsmdb::consensus::StorageStateMachine>(node.storage);
  node.raft = std::make_shared<lsmdb::consensus::RaftNode>(
      index + 1, peers, node.state_machine, root / "raft" / std::to_string(index + 1),
      node.persistent->store().LastSequence());
  node.backend = std::make_shared<lsmdb::consensus::ReplicatedBackend>(node.raft, node.storage);
  lsmdb::rpc::NodeOptions options;
  options.node_id = index + 1;
  options.advertise_host = "127.0.0.1";
  options.port = ports[index];
  options.peers = peers;
  node.server = std::make_unique<lsmdb::rpc::NodeServer>(options, node.backend, node.raft);
  node.server->Start();
  node.raft->Start();
}

void StopNode(Node& node) {
  if (node.server) node.server->Shutdown();
  if (node.raft) node.raft->Stop();
  node.server.reset();
  node.backend.reset();
  node.raft.reset();
  node.state_machine.reset();
  node.storage.reset();
  node.persistent.reset();
}
}  // namespace

TEST(RaftCluster, ElectsLeaderReplicatesWritesAndReelectsAfterLeaderLoss) {
  const fs::path root = fs::temp_directory_path() / ("lsmdb-raft-cluster-" + std::to_string(::getpid()));
  fs::remove_all(root);
  std::array<std::uint32_t, 3> ports{};
  for (int i = 0; i < 3; ++i) {
    do { ports[i] = ReservePort(); }
    while (std::find(ports.begin(), ports.begin() + i, ports[i]) != ports.begin() + i);
  }
  std::array<Node, 3> nodes;
  Cleanup cleanup{&nodes, root};
  try {
    for (int i = 0; i < 3; ++i) {
      StartNode(nodes[i], i, root, ports);
    }
    ASSERT_TRUE(WaitForLeader(nodes));
    int leader = -1;
    for (int i = 0; i < 3; ++i) if (nodes[i].raft->IsLeader()) leader = i;
    ASSERT_GE(leader, 0);

    nodes[leader].backend->Put("cluster-key", "before-failure");
    ASSERT_TRUE(WaitForReplicatedValue(nodes, "before-failure"));
    EXPECT_EQ(nodes[leader].backend->Get("cluster-key"), std::optional<std::string>("before-failure"));
    EXPECT_THROW((void)nodes[(leader + 1) % 3].backend->Get("cluster-key"),
                 lsmdb::consensus::NotLeaderError);
    for (const auto& node : nodes) {
      ASSERT_EQ(node.storage->Get("cluster-key"), std::optional<std::string>("before-failure"));
      EXPECT_EQ(node.raft->CommitIndex(), node.raft->LastApplied());
    }
    int stopped_follower = (leader + 1) % 3;
    StopNode(nodes[stopped_follower]);
    nodes[leader].backend->Put("quorum-key", "one-follower-down");
    EXPECT_EQ(nodes[leader].storage->Get("quorum-key"), std::optional<std::string>("one-follower-down"));
    int available_follower = 3 - leader - stopped_follower;
    const auto avail_end = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < avail_end &&
           nodes[available_follower].storage->Get("quorum-key") !=
               std::optional<std::string>("one-follower-down")) std::this_thread::sleep_for(20ms);
    EXPECT_EQ(nodes[available_follower].storage->Get("quorum-key"),
              std::optional<std::string>("one-follower-down"));
    StartNode(nodes[stopped_follower], stopped_follower, root, ports);
    const auto replication_end = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < replication_end &&
           nodes[stopped_follower].storage->Get("quorum-key") !=
               std::optional<std::string>("one-follower-down")) std::this_thread::sleep_for(20ms);
    ASSERT_EQ(nodes[stopped_follower].storage->Get("quorum-key"),
              std::optional<std::string>("one-follower-down"));

    // A leader isolated from both followers must not commit or apply another
    // proposal. Once connectivity returns, Raft may safely commit that pending
    // entry; the client has already received UNAVAILABLE for the timed-out call.
    StopNode(nodes[stopped_follower]);
    StopNode(nodes[available_follower]);
    const auto commit_before_quorum_loss = nodes[leader].raft->CommitIndex();
    EXPECT_THROW(nodes[leader].backend->Put("quorum-loss-key", "pending"),
                 lsmdb::consensus::ConsensusUnavailableError);
    EXPECT_EQ(nodes[leader].raft->CommitIndex(), commit_before_quorum_loss);
    EXPECT_FALSE(nodes[leader].storage->Get("quorum-loss-key"));

    StartNode(nodes[available_follower], available_follower, root, ports);
    StartNode(nodes[stopped_follower], stopped_follower, root, ports);
    const auto quorum_restore_end = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < quorum_restore_end &&
           nodes[available_follower].storage->Get("quorum-loss-key") !=
               std::optional<std::string>("pending")) std::this_thread::sleep_for(20ms);
    ASSERT_EQ(nodes[available_follower].storage->Get("quorum-loss-key"),
              std::optional<std::string>("pending"));
    ASSERT_EQ(nodes[leader].storage->Get("quorum-loss-key"),
              std::optional<std::string>("pending"));

    const auto old_term = nodes[leader].raft->CurrentTerm();
    StopNode(nodes[leader]);
    ASSERT_TRUE(WaitForLeader(nodes, leader));

    int replacement = -1;
    for (int i = 0; i < 3; ++i)
      if (i != leader && nodes[i].raft->IsLeader()) replacement = i;
    ASSERT_GE(replacement, 0);
    EXPECT_GT(nodes[replacement].raft->CurrentTerm(), old_term);
    EXPECT_EQ(nodes[replacement].backend->Get("cluster-key"),
              std::optional<std::string>("before-failure"));
    nodes[replacement].backend->Put("cluster-key", "after-failure");
    ASSERT_TRUE(WaitForReplicatedValue(nodes, "after-failure", leader));
    for (int i = 0; i < 3; ++i) if (i != leader) {
      EXPECT_EQ(nodes[i].storage->Get("cluster-key"), std::optional<std::string>("after-failure"));
      EXPECT_EQ(nodes[i].raft->CommitIndex(), nodes[i].raft->LastApplied());
    }
  } catch (...) {
    throw;
  }
}
