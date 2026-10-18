// Command format and validation tests (Week 5): round trips, strict decoding, limits.
#include <gtest/gtest.h>

#include <random>

#include "lsmdb/consensus/command.h"

using namespace lsmdb::consensus;

namespace {
Command RoundTrip(const Command& c) {
  DecodeResult d = Decode(Encode(c));
  EXPECT_TRUE(d) << d.error;
  return d ? *d.command : Command{};
}
}  // namespace

TEST(CommandCodec, PutRoundTrip) {
  Command c = Command::Put("user:1", "alice", "req-1");
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, DeleteRoundTrip) {
  Command c = Command::Delete("user:1", "req-2");
  Command r = RoundTrip(c);
  EXPECT_EQ(r, c);
  EXPECT_EQ(r.type, CommandType::kDelete);
  EXPECT_TRUE(r.value.empty());
}

TEST(CommandCodec, InternalNoopRoundTripsWithoutStorageFields) {
  const Command c = Command::Noop();
  EXPECT_TRUE(Validate(c));
  EXPECT_EQ(RoundTrip(c), c);
  Command malformed = c;
  malformed.key = "not-empty";
  EXPECT_FALSE(Validate(malformed));
}

TEST(CommandCodec, BinarySafeKeysAndValues) {
  Command c = Command::Put(std::string("a\0b\xff\xfe", 5), std::string("\0\0\0\x01\0", 5), std::string("\0", 1));
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, EmptyValueAndEmptyRequestIdAreValid) {
  Command c = Command::Put("k", "");
  EXPECT_TRUE(Validate(c));
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, MaximumSizeCommandRoundTripsWithinDeclaredBound) {
  Command c = Command::Put(std::string(kMaxKeyBytes, 'k'), std::string(kMaxValueBytes, 'v'),
                           std::string(kMaxRequestIdBytes, 'r'));
  EXPECT_TRUE(Validate(c));
  std::string bytes = Encode(c);
  EXPECT_EQ(bytes.size(), kMaxEncodedCommandBytes);
  EXPECT_EQ(RoundTrip(c), c);
}

TEST(CommandCodec, EncodingIsDeterministicAndLayoutIsStable) {
  // Pin the byte layout: changing it would break logs written by older nodes.
  Command c = Command::Put("ab", "xyz", "q");
  std::string expected;
  expected += '\x01';                       // version
  expected += '\x01';                       // type put
  expected += std::string("\x02\0\0\0", 4); // key_len
  expected += std::string("\x03\0\0\0", 4); // value_len
  expected += std::string("\x01\0\0\0", 4); // request_id_len
  expected += "abxyzq";
  EXPECT_EQ(Encode(c), expected);
  EXPECT_EQ(Encode(c), Encode(c));
}

TEST(CommandCodec, EveryTruncationIsRejected) {
  std::string good = Encode(Command::Put("key", "value", "id"));
  for (std::size_t n = 0; n < good.size(); ++n) {
    DecodeResult d = Decode(good.substr(0, n));
    EXPECT_FALSE(d) << "prefix of length " << n << " must not decode";
    EXPECT_FALSE(d.error.empty());
  }
}

TEST(CommandCodec, TrailingBytesAreRejected) {
  std::string good = Encode(Command::Put("key", "value"));
  EXPECT_FALSE(Decode(good + "x"));
}

TEST(CommandCodec, UnknownVersionAndTypeAreRejected) {
  std::string good = Encode(Command::Put("k", "v"));
  std::string bad_version = good;
  bad_version[0] = 2;
  EXPECT_FALSE(Decode(bad_version));
  for (int t : {0, 4, 255}) {
    std::string bad_type = good;
    bad_type[1] = static_cast<char>(t);
    EXPECT_FALSE(Decode(bad_type)) << "type " << t;
  }
}

TEST(CommandCodec, LengthFieldsThatLieAreRejectedWithoutOverreading) {
  std::string good = Encode(Command::Put("k", "v"));
  std::string huge = good;
  for (int i = 2; i < 14; ++i) huge[i] = '\xff';  // all three lengths ~4 GiB
  EXPECT_FALSE(Decode(huge));
  std::string off_by_one = good;
  off_by_one[2] = 2;  // key_len says 2, buffer holds 1
  EXPECT_FALSE(Decode(off_by_one));
}

TEST(CommandCodec, RandomBytesNeverCrashAndMutationsAreCaughtOrStillValid) {
  std::mt19937 rng(42);
  std::string good = Encode(Command::Put("some-key", "some-value", "rid"));
  for (int i = 0; i < 5000; ++i) {
    std::string s(rng() % 40, '\0');
    for (auto& ch : s) ch = static_cast<char>(rng());
    (void)Decode(s);  // must simply return
    std::string m = good;
    m[rng() % m.size()] ^= static_cast<char>(1 + rng() % 255);
    DecodeResult d = Decode(m);
    if (d) {
      EXPECT_NE(*d.command, *Decode(good).command);  // a decodable mutation is a different command
    }
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
