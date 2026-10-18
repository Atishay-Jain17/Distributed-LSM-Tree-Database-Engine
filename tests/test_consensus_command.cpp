#include "test_framework.h"
#include "lsmdb/consensus/command.h"

using namespace lsmdb::consensus;

TEST_CASE(TestConsensusCommandCodecRoundTrip) {
    Command put_cmd = Command::Put("user:100", "Alice", "req-1");
    std::string encoded_put = Encode(put_cmd);
    DecodeResult decoded_put = Decode(encoded_put);
    ASSERT_TRUE(static_cast<bool>(decoded_put));
    ASSERT_TRUE(decoded_put.error.empty());
    ASSERT_TRUE(decoded_put.command.has_value());
    ASSERT_TRUE(*decoded_put.command == put_cmd);

    Command del_cmd = Command::Delete("user:100", "req-2");
    std::string encoded_del = Encode(del_cmd);
    DecodeResult decoded_del = Decode(encoded_del);
    ASSERT_TRUE(static_cast<bool>(decoded_del));
    ASSERT_TRUE(decoded_del.error.empty());
    ASSERT_TRUE(decoded_del.command.has_value());
    ASSERT_TRUE(*decoded_del.command == del_cmd);
    ASSERT_TRUE(decoded_del.command->type == CommandType::kDelete);
    ASSERT_TRUE(decoded_del.command->value.empty());

    Command noop_cmd = Command::Noop();
    std::string encoded_noop = Encode(noop_cmd);
    DecodeResult decoded_noop = Decode(encoded_noop);
    ASSERT_TRUE(static_cast<bool>(decoded_noop));
    ASSERT_TRUE(decoded_noop.error.empty());
    ASSERT_TRUE(decoded_noop.command.has_value());
    ASSERT_TRUE(*decoded_noop.command == noop_cmd);
}

TEST_CASE(TestConsensusCommandValidation) {
    Command valid_put = Command::Put("valid_key", "valid_value", "req-1");
    ASSERT_TRUE(Validate(valid_put).ok);

    Command empty_key = Command::Put("", "val", "req-2");
    ASSERT_FALSE(Validate(empty_key).ok);

    Command del_with_val = Command::Delete("del_key", "req-3");
    del_with_val.value = "unexpected";
    ASSERT_FALSE(Validate(del_with_val).ok);

    Command noop_with_key = Command::Noop();
    noop_with_key.key = "noop_key";
    ASSERT_FALSE(Validate(noop_with_key).ok);

    std::string binary_key("bin\0key", 7);
    std::string binary_val("bin\0val", 7);
    Command binary_cmd = Command::Put(binary_key, binary_val, "req-bin");
    ASSERT_TRUE(Validate(binary_cmd).ok);
    DecodeResult decoded_binary = Decode(Encode(binary_cmd));
    ASSERT_TRUE(static_cast<bool>(decoded_binary));
    ASSERT_TRUE(decoded_binary.command.has_value());
    ASSERT_EQ(decoded_binary.command->key, binary_key);
    ASSERT_EQ(decoded_binary.command->value, binary_val);
}
