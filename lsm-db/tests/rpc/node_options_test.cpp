#include <gtest/gtest.h>

#include <stdexcept>

#include "lsmdb/rpc/node_options.h"

using namespace lsmdb;
using namespace lsmdb::rpc;

TEST(ParsePeers, ParsesTwoPeers) {
  auto peers = ParsePeers("2=127.0.0.1:50052, 3=lsmdb-2.lsmdb.svc:50053");
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0].node_id(), 2u);
  EXPECT_EQ(peers[0].host(), "127.0.0.1");
  EXPECT_EQ(peers[0].port(), 50052u);
  EXPECT_EQ(peers[1].host(), "lsmdb-2.lsmdb.svc");
}

TEST(ParsePeers, EmptyMeansNoPeers) {
  EXPECT_TRUE(ParsePeers("").empty());
  EXPECT_TRUE(ParsePeers("   ").empty());
}

TEST(ParsePeers, RejectsMalformedInput) {
  for (const char* bad : {"2", "2=host", "=host:1", "x=host:1", "0=host:1", "2=:50052",
                          "2=host:0", "2=host:70000", "2=host:abc", "2=host:1,", "2=host:1,2=h:2"}) {
    EXPECT_THROW(ParsePeers(bad), std::invalid_argument) << "input: " << bad;
  }
}

TEST(ValidateNodeOptions, AcceptsGoodOptions) {
  NodeOptions o;
  o.node_id = 1;
  o.port = 50051;
  o.peers = ParsePeers("2=h:50052,3=h:50053");
  EXPECT_NO_THROW(ValidateNodeOptions(o));
}

TEST(ValidateNodeOptions, RejectsBadOptions) {
  NodeOptions o;
  o.port = 50051;
  EXPECT_THROW(ValidateNodeOptions(o), std::invalid_argument);  // node_id 0

  o.node_id = 1;
  o.port = 0;
  EXPECT_THROW(ValidateNodeOptions(o), std::invalid_argument);  // port 0 not allowed
  EXPECT_NO_THROW(ValidateNodeOptions(o, /*allow_ephemeral_port=*/true));

  o.port = 50051;
  o.peers = ParsePeers("1=h:50052");
  EXPECT_THROW(ValidateNodeOptions(o), std::invalid_argument);  // peer is self
}
