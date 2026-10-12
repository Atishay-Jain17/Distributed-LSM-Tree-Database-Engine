#pragma once
// The replicated command (Week 5): the unit of data Raft copies between nodes and that every node
// applies to its storage in the same order. See docs/consensus-boundary.md.
//
// Wire/log form: unified on protobuf lsmdb.v1.ReplicatedCommand (single wire format).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace lsmdb::consensus_boundary {

enum class CommandType : std::uint8_t { kPut = 1, kDelete = 2 };

struct Command {
  CommandType type = CommandType::kPut;
  std::string key;
  std::string value;       // must be empty for kDelete
  std::string request_id;  // caller-chosen id (tracing / future de-duplication); retained in-memory

  static Command Put(std::string key, std::string value, std::string request_id = {}) {
    return {CommandType::kPut, std::move(key), std::move(value), std::move(request_id)};
  }
  static Command Delete(std::string key, std::string request_id = {}) {
    return {CommandType::kDelete, std::move(key), {}, std::move(request_id)};
  }
  bool operator==(const Command&) const = default;
};

// Same limits as the RPC layer (rpc/validation.h); a static_assert/test keeps them equal.
inline constexpr std::size_t kMaxKeyBytes = 1024;
inline constexpr std::size_t kMaxValueBytes = 1024 * 1024;
inline constexpr std::size_t kMaxRequestIdBytes = 128;
// Small fixed slack for protobuf field tags and varint length headers.
inline constexpr std::size_t kProtobufSlackBytes = 64;
inline constexpr std::size_t kMaxEncodedCommandBytes =
    kMaxKeyBytes + kMaxValueBytes + kMaxRequestIdBytes + kProtobufSlackBytes;

struct ValidationResult {
  bool ok = true;
  std::string message;  // empty when ok
  explicit operator bool() const { return ok; }
};

// Checks a command is well formed AND within limits. Used before proposing and again before
// applying, so storage only ever sees valid commands.
ValidationResult Validate(const Command& cmd);

// Serialises a command using protobuf v1::ReplicatedCommand. Does not validate (call Validate first); never throws for valid sizes.
std::string Encode(const Command& cmd);

struct DecodeResult {
  std::optional<Command> command;  // set on success
  std::string error;               // set on failure
  explicit operator bool() const { return command.has_value(); }
};

// Parses bytes produced by Encode() using protobuf v1::ReplicatedCommand. Strict: rejects truncated/corrupt input,
// unknown or unspecified type, empty key, or delete with non-empty value.
DecodeResult Decode(const std::string& bytes);

}  // namespace lsmdb::consensus_boundary
