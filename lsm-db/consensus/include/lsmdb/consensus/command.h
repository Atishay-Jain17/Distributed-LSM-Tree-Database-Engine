#pragma once
// The replicated command (Week 5): the unit of data Raft copies between nodes and that every node
// applies to its storage in the same order. See docs/consensus-boundary.md.
//
// Wire/log form (version 1, little-endian, produced by Encode()):
//   u8 version(=1) | u8 type | u32 key_len | u32 value_len | u32 request_id_len
//   | key | value | request_id
// The Raft log stores these bytes as the entry payload; AppendEntries carries them in a `bytes`
// field. Nothing here depends on gRPC or protobuf.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace lsmdb::consensus {

enum class CommandType : std::uint8_t { kPut = 1, kDelete = 2, kNoop = 3 };

struct Command {
  CommandType type = CommandType::kPut;
  std::string key;
  std::string value;       // must be empty for kDelete
  std::string request_id;  // caller-chosen id (tracing / future de-duplication); may be empty

  static Command Put(std::string key, std::string value, std::string request_id = {}) {
    return {CommandType::kPut, std::move(key), std::move(value), std::move(request_id)};
  }
  static Command Delete(std::string key, std::string request_id = {}) {
    return {CommandType::kDelete, std::move(key), {}, std::move(request_id)};
  }
  static Command Noop() { return {CommandType::kNoop, {}, {}, {}}; }
  bool operator==(const Command&) const = default;
};

// Same limits as the RPC layer (rpc/validation.h); a test keeps them equal.
inline constexpr std::size_t kMaxKeyBytes = 1024;
inline constexpr std::size_t kMaxValueBytes = 1024 * 1024;
inline constexpr std::size_t kMaxRequestIdBytes = 128;
inline constexpr std::uint8_t kCommandFormatVersion = 1;
inline constexpr std::size_t kCommandHeaderBytes = 14;
inline constexpr std::size_t kMaxEncodedCommandBytes =
    kCommandHeaderBytes + kMaxKeyBytes + kMaxValueBytes + kMaxRequestIdBytes;

struct ValidationResult {
  bool ok = true;
  std::string message;  // empty when ok
  explicit operator bool() const { return ok; }
};

// Checks a command is well formed AND within limits. Used before proposing and again before
// applying, so storage only ever sees valid commands.
ValidationResult Validate(const Command& cmd);

// Serialises a command. Does not validate (call Validate first); never throws for valid sizes.
std::string Encode(const Command& cmd);

struct DecodeResult {
  std::optional<Command> command;  // set on success
  std::string error;               // set on failure
  explicit operator bool() const { return command.has_value(); }
};

// Parses bytes produced by Encode(). Strict: rejects truncated input, trailing bytes, unknown
// version or type, and lengths that do not match the buffer. Does not check size limits
// (Validate does). Never reads out of bounds, whatever the input.
DecodeResult Decode(const std::string& bytes);

}  // namespace lsmdb::consensus
