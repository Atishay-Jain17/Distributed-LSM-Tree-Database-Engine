// Command format and validation tests (Week 5): protobuf round trips, strict decoding, limits, interoperability.
#include <gtest/gtest.h>

#include <random>

#include "lsmdb/consensus/command.h"
#include "lsmdb/consensus_boundary/command.h"
#include "lsmdb/v1/command.pb.h"

using namespace lsmdb::consensus_boundary;

namespace {
Command RoundTrip(const Command& c) {
  DecodeResult d = Decode(Encode(c));
  EXPECT_TRUE(d) << d.error;
  return d ? *d.command : Command{};
}
}  // namespace

TEST(CommandCodec, PutRoundTrip) {
  Command c = Command::Put("user:1", "alice");
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, DeleteRoundTrip) {
  Command c = Command::Delete("user:1");
  Command r = RoundTrip(c);
  EXPECT_EQ(r, c);
  EXPECT_EQ(r.type, CommandType::kDelete);
  EXPECT_TRUE(r.value.empty());
}

TEST(CommandCodec, RequestIdWireGapDocumented) {
  // Marked gap: request_id is retained in Command struct but dropped on protobuf wire until Vinayak merges Request #1.
  Command c = Command::Put("user:1", "alice", "req-1");
  Command r = RoundTrip(c);
  EXPECT_EQ(r.key, c.key);
  EXPECT_EQ(r.value, c.value);
  EXPECT_EQ(r.type, c.type);
  EXPECT_EQ(r.request_id, "");  // not serialized on wire yet
}

TEST(CommandCodec, BinarySafeKeysAndValues) {
  Command c = Command::Put(std::string("a\0b\xff\xfe", 5), std::string("\0\0\0\x01\0", 5));
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, EmptyValueAndEmptyRequestIdAreValid) {
  Command c = Command::Put("k", "");
  EXPECT_TRUE(Validate(c));
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, MaximumSizeCommandRoundTripsWithinDeclaredBound) {
  Command c = Command::Put(std::string(kMaxKeyBytes, 'k'), std::string(kMaxValueBytes, 'v'));
  EXPECT_TRUE(Validate(c));
  std::string bytes = Encode(c);
  EXPECT_LE(bytes.size(), kMaxEncodedCommandBytes);
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, EncodingIsDeterministic) {
  Command c = Command::Put("ab", "xyz");
  EXPECT_EQ(Encode(c), Encode(c));
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, RejectsEmptyPayload) {
  DecodeResult d = Decode("");
  EXPECT_FALSE(d);
  EXPECT_FALSE(d.error.empty());
}

TEST(CommandCodec, RejectsCorruptProtobuf) {
  DecodeResult d = Decode("\xff\xff\xff\xff");
  EXPECT_FALSE(d);
  EXPECT_FALSE(d.error.empty());
}

TEST(CommandCodec, RejectsUnknownOrUnspecifiedCommandType) {
  lsmdb::v1::ReplicatedCommand proto;
  proto.set_type(lsmdb::v1::COMMAND_TYPE_UNSPECIFIED);
  proto.set_key("k");
  std::string wire;
  proto.SerializeToString(&wire);
  DecodeResult d = Decode(wire);
  EXPECT_FALSE(d);
}

TEST(CommandCodec, RejectsEmptyKeyInProto) {
  lsmdb::v1::ReplicatedCommand proto;
  proto.set_type(lsmdb::v1::COMMAND_TYPE_PUT);
  proto.set_key("");
  proto.set_value("v");
  std::string wire;
  proto.SerializeToString(&wire);
  DecodeResult d = Decode(wire);
  ASSERT_TRUE(d);
  EXPECT_FALSE(Validate(*d.command));
}

TEST(CommandCodec, RejectsDeleteWithValueInProto) {
  lsmdb::v1::ReplicatedCommand proto;
  proto.set_type(lsmdb::v1::COMMAND_TYPE_DELETE);
  proto.set_key("k");
  proto.set_value("non-empty-value");
  std::string wire;
  proto.SerializeToString(&wire);
  DecodeResult d = Decode(wire);
  ASSERT_TRUE(d);
  EXPECT_FALSE(Validate(*d.command));
}

TEST(CommandCodec, InteroperabilityWithVinayakConsensusCodec) {
  // 1. My Encode() output must decode with Vinayak's DecodeCommand()
  Command my_put = Command::Put("alpha", "123");
  std::string my_encoded = Encode(my_put);
  auto v_decoded = lsmdb::consensus::DecodeCommand(my_encoded);
  ASSERT_TRUE(v_decoded.has_value());
  EXPECT_EQ(v_decoded->type(), lsmdb::v1::COMMAND_TYPE_PUT);
  EXPECT_EQ(v_decoded->key(), "alpha");
  EXPECT_EQ(v_decoded->value(), "123");

  Command my_del = Command::Delete("gamma");
  std::string my_del_encoded = Encode(my_del);
  auto v_del_decoded = lsmdb::consensus::DecodeCommand(my_del_encoded);
  ASSERT_TRUE(v_del_decoded.has_value());
  EXPECT_EQ(v_del_decoded->type(), lsmdb::v1::COMMAND_TYPE_DELETE);
  EXPECT_EQ(v_del_decoded->key(), "gamma");

  // 2. Vinayak's EncodeCommand() output must decode with my Decode()
  auto v_cmd = lsmdb::consensus::MakePutCommand("beta", "456");
  std::string v_encoded = lsmdb::consensus::EncodeCommand(v_cmd);
  DecodeResult my_decoded = Decode(v_encoded);
  ASSERT_TRUE(my_decoded);
  EXPECT_EQ(my_decoded.command->type, CommandType::kPut);
  EXPECT_EQ(my_decoded.command->key, "beta");
  EXPECT_EQ(my_decoded.command->value, "456");

  auto v_del_cmd = lsmdb::consensus::MakeDeleteCommand("delta");
  std::string v_del_encoded = lsmdb::consensus::EncodeCommand(v_del_cmd);
  DecodeResult my_del_decoded = Decode(v_del_encoded);
  ASSERT_TRUE(my_del_decoded);
  EXPECT_EQ(my_del_decoded.command->type, CommandType::kDelete);
  EXPECT_EQ(my_del_decoded.command->key, "delta");
  EXPECT_TRUE(my_del_decoded.command->value.empty());
}

TEST(CommandCodec, RandomBytesNeverCrash) {
  std::mt19937 rng(42);
  for (int i = 0; i < 5000; ++i) {
    std::string s(rng() % 64, '\0');
    for (auto& ch : s) ch = static_cast<char>(rng());
    (void)Decode(s);  // must never crash
  }
}

TEST(CommandValidation, AcceptsNormalCommands) {
  EXPECT_TRUE(Validate(Command::Put("k", "v")));
  EXPECT_TRUE(Validate(Command::Delete("k")));
}

TEST(CommandValidation, RejectsEmptyAndOversizedKey) {
  EXPECT_FALSE(Validate(Command::Put("", "v")));
  EXPECT_FALSE(Validate(Command::Delete("")));
  EXPECT_TRUE(Validate(Command::Put(std::string(kMaxKeyBytes, 'k'), "v")));
  EXPECT_FALSE(Validate(Command::Put(std::string(kMaxKeyBytes + 1, 'k'), "v")));
}

TEST(CommandValidation, RejectsOversizedValueAndRequestId) {
  EXPECT_TRUE(Validate(Command::Put("k", std::string(kMaxValueBytes, 'v'))));
  EXPECT_FALSE(Validate(Command::Put("k", std::string(kMaxValueBytes + 1, 'v'))));
  EXPECT_FALSE(Validate(Command::Put("k", "v", std::string(kMaxRequestIdBytes + 1, 'r'))));
}

TEST(CommandValidation, DeleteMustNotCarryAValueAndTypeMustBeKnown) {
  Command d = Command::Delete("k");
  d.value = "oops";
  ValidationResult r = Validate(d);
  EXPECT_FALSE(r);
  EXPECT_FALSE(r.message.empty());
  Command bad;
  bad.type = static_cast<CommandType>(9);
  bad.key = "k";
  EXPECT_FALSE(Validate(bad));
}
