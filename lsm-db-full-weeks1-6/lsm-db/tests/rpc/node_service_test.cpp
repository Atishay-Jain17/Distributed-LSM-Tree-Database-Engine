// Node identity and node-to-node message tests (three nodes, distinct identities).
#include <gtest/gtest.h>

#include <set>

#include "lsmdb/rpc/status.h"
#include "lsmdb/rpc/validation.h"
#include "test_support.h"

using namespace lsmdb;
using namespace lsmdb::rpc;
using lsmdb::testing_support::MakeNodeInfo;
using lsmdb::testing_support::TestNode;

TEST(NodeService, ThreeNodesHaveDistinctIdentitiesAndPorts) {
  TestNode n1(1, {MakeNodeInfo(2, "10.0.0.2", 50052), MakeNodeInfo(3, "10.0.0.3", 50053)});
  TestNode n2(2, {MakeNodeInfo(1, "10.0.0.1", 50051), MakeNodeInfo(3, "10.0.0.3", 50053)});
  TestNode n3(3, {MakeNodeInfo(1, "10.0.0.1", 50051), MakeNodeInfo(2, "10.0.0.2", 50052)});

  std::set<std::uint32_t> ids, ports;
  for (TestNode* n : {&n1, &n2, &n3}) {
    auto pong = NodeClient(n->channel()).Ping();
    ASSERT_TRUE(IsOk(pong.status));
    ids.insert(pong.responder.node_id());
    ports.insert(pong.responder.port());
    EXPECT_EQ(pong.responder.port(), n->port());  // reports the real bound port
  }
  EXPECT_EQ(ids, (std::set<std::uint32_t>{1, 2, 3}));
  EXPECT_EQ(ports.size(), 3u);
}

TEST(NodeService, GetNodeInfoReturnsSelfAndConfiguredPeers) {
  TestNode node(2, {MakeNodeInfo(1, "10.0.0.1", 50051), MakeNodeInfo(3, "10.0.0.3", 50053)});
  auto info = NodeClient(node.channel()).GetNodeInfo();
  ASSERT_TRUE(IsOk(info.status));
  EXPECT_EQ(info.self.node_id(), 2u);
  ASSERT_EQ(info.peers.size(), 2u);
  EXPECT_EQ(info.peers[0].node_id(), 1u);
  EXPECT_EQ(info.peers[1].host(), "10.0.0.3");
}

TEST(NodeService, StorageIsIndependentPerNodeUntilRaftExists) {
  TestNode n1(1), n2(2);
  ASSERT_TRUE(IsOk(KvClient(n1.channel()).Put("k", "v")));
  EXPECT_EQ(KvClient(n1.channel()).Get("k").status.code(), v1::STATUS_CODE_OK);
  EXPECT_EQ(KvClient(n2.channel()).Get("k").status.code(), v1::STATUS_CODE_NOT_FOUND);
}

TEST(NodeService, NodeToNodeCallCarriesSenderId) {
  TestNode node(2);
  ClientOptions opts;
  opts.sender_node_id = 1;  // pretend node 1 is calling node 2
  auto pong = NodeClient(node.channel(), opts).Ping();
  ASSERT_TRUE(IsOk(pong.status));
  EXPECT_EQ(pong.responder.node_id(), 2u);
}

TEST(NodeService, PingRejectsOversizedRequestId) {
  TestNode node(1);
  auto stub = v1::NodeService::NewStub(node.channel());
  v1::PingRequest req;
  req.mutable_meta()->set_request_id(std::string(kMaxRequestIdBytes + 1, 'x'));
  v1::PingResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->Ping(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.status().code(), v1::STATUS_CODE_INVALID_ARGUMENT);
}

TEST(NodeService, DeadPeerReportsUnavailable) {
  std::string target;
  {
    TestNode node(2);
    target = node.target();
  }
  ClientOptions opts;
  opts.deadline = std::chrono::milliseconds(2000);
  EXPECT_EQ(NodeClient(MakeChannel(target), opts).Ping().status.code(),
            v1::STATUS_CODE_UNAVAILABLE);
}

TEST(NodeServer, StartingTwiceOnSamePortFailsClearly) {
  TestNode first(1);
  NodeOptions opts;
  opts.node_id = 2;
  opts.port = first.port();  // already taken
  NodeServer second(opts, std::make_shared<InMemoryBackend>());
  EXPECT_THROW(second.Start(), std::runtime_error);
}
