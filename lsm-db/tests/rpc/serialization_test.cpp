// Wire-format tests: what we serialize must parse back identically, and the
// contract must stay evolvable. Covers client->node and node->node messages.
#include <gtest/gtest.h>

#include "lsmdb/v1/kv.pb.h"
#include "lsmdb/v1/node.pb.h"

using namespace lsmdb;

namespace {
template <typename T>
T RoundTrip(const T& in) {
  std::string wire;
  EXPECT_TRUE(in.SerializeToString(&wire));
  T out;
  EXPECT_TRUE(out.ParseFromString(wire));
  return out;
}
}  // namespace

TEST(Serialization, PutRequestPreservesBinaryKeyAndValue) {
  v1::PutRequest in;
  in.mutable_meta()->set_request_id("req-1");
  in.mutable_meta()->set_sender_node_id(2);
  in.mutable_meta()->set_client_timestamp_ms(1780000000123);
  in.set_key(std::string("a\0b\xff\xfe", 5));
  in.set_value(std::string("\0\0\0\x01", 4));

  auto out = RoundTrip(in);
  EXPECT_EQ(out.key(), in.key());
  EXPECT_EQ(out.value(), in.value());
  EXPECT_EQ(out.meta().request_id(), "req-1");
  EXPECT_EQ(out.meta().sender_node_id(), 2u);
  EXPECT_EQ(out.meta().client_timestamp_ms(), 1780000000123);
}

TEST(Serialization, LargeValueRoundTrip) {
  v1::PutRequest in;
  in.set_key("big");
  in.set_value(std::string(1024 * 1024, 'z'));
  EXPECT_EQ(RoundTrip(in).value().size(), 1024u * 1024u);
}

TEST(Serialization, DefaultStatusIsOk) {
  v1::Status s;
  EXPECT_EQ(s.code(), v1::STATUS_CODE_OK);
  v1::PutResponse r;  // status field never set
  EXPECT_EQ(RoundTrip(r).status().code(), v1::STATUS_CODE_OK);
}

TEST(Serialization, NotLeaderStatusCarriesLeaderHint) {
  v1::GetResponse in;
  in.mutable_status()->set_code(v1::STATUS_CODE_NOT_LEADER);
  in.mutable_status()->set_message("redirect");
  auto* hint = in.mutable_status()->mutable_leader_hint();
  hint->set_node_id(3);
  hint->set_host("lsmdb-2.lsmdb");
  hint->set_port(50051);

  auto out = RoundTrip(in);
  EXPECT_EQ(out.status().code(), v1::STATUS_CODE_NOT_LEADER);
  ASSERT_TRUE(out.status().has_leader_hint());
  EXPECT_EQ(out.status().leader_hint().node_id(), 3u);
  EXPECT_EQ(out.status().leader_hint().host(), "lsmdb-2.lsmdb");
}

TEST(Serialization, NodeToNodePingAndInfoMessages) {
  v1::PingResponse ping;
  ping.mutable_responder()->set_node_id(2);
  ping.mutable_responder()->set_host("10.0.0.2");
  ping.mutable_responder()->set_port(50052);
  EXPECT_EQ(RoundTrip(ping).responder().node_id(), 2u);

  v1::GetNodeInfoResponse info;
  info.mutable_self()->set_node_id(1);
  for (std::uint32_t id : {2u, 3u}) {
    auto* p = info.add_peers();
    p->set_node_id(id);
    p->set_host("10.0.0." + std::to_string(id));
    p->set_port(50050 + id);
  }
  auto out = RoundTrip(info);
  ASSERT_EQ(out.peers_size(), 2);
  EXPECT_EQ(out.peers(0).node_id(), 2u);
  EXPECT_EQ(out.peers(1).port(), 50053u);
}

TEST(Serialization, GarbageBytesAreRejected) {
  v1::PutRequest out;
  EXPECT_FALSE(out.ParseFromString(std::string("\xff\xff\xff", 3)));
}

TEST(Serialization, UnknownFieldsFromNewerPeersAreIgnored) {
  // A newer node adds field 15 (varint). An older node must still parse the rest.
  v1::PutRequest in;
  in.set_key("k");
  in.set_value("v");
  std::string wire;
  ASSERT_TRUE(in.SerializeToString(&wire));
  wire += std::string("\x78\x01", 2);  // tag (15 << 3 | 0), value 1

  v1::PutRequest out;
  ASSERT_TRUE(out.ParseFromString(wire));
  EXPECT_EQ(out.key(), "k");
  EXPECT_EQ(out.value(), "v");
}

TEST(Serialization, StatusCodeNumbersAreFrozen) {
  // These numbers are the wire contract with the other three members.
  EXPECT_EQ(static_cast<int>(v1::STATUS_CODE_OK), 0);
  EXPECT_EQ(static_cast<int>(v1::STATUS_CODE_INVALID_ARGUMENT), 1);
  EXPECT_EQ(static_cast<int>(v1::STATUS_CODE_NOT_FOUND), 2);
  EXPECT_EQ(static_cast<int>(v1::STATUS_CODE_NOT_LEADER), 3);
  EXPECT_EQ(static_cast<int>(v1::STATUS_CODE_UNAVAILABLE), 4);
  EXPECT_EQ(static_cast<int>(v1::STATUS_CODE_INTERNAL), 5);
}
