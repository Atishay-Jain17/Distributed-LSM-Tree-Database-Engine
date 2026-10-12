#include "lsmdb/consensus_boundary/command.h"

#include <limits>

namespace lsmdb::consensus_boundary {
namespace {

void PutU32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

std::uint32_t GetU32(const std::string& s, std::size_t pos) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[pos + i])) << (8 * i);
  return v;
}

ValidationResult Fail(std::string m) { return {false, std::move(m)}; }

}  // namespace

ValidationResult Validate(const Command& cmd) {
  if (cmd.type != CommandType::kPut && cmd.type != CommandType::kDelete)
    return Fail("unknown command type");
  if (cmd.key.empty()) return Fail("key must not be empty");
  if (cmd.key.size() > kMaxKeyBytes) return Fail("key exceeds " + std::to_string(kMaxKeyBytes) + " bytes");
  if (cmd.value.size() > kMaxValueBytes)
    return Fail("value exceeds " + std::to_string(kMaxValueBytes) + " bytes");
  if (cmd.type == CommandType::kDelete && !cmd.value.empty())
    return Fail("delete command must not carry a value");
  if (cmd.request_id.size() > kMaxRequestIdBytes)
    return Fail("request_id exceeds " + std::to_string(kMaxRequestIdBytes) + " bytes");
  return {};
}

std::string Encode(const Command& cmd) {
  std::string out;
  out.reserve(kCommandHeaderBytes + cmd.key.size() + cmd.value.size() + cmd.request_id.size());
  out.push_back(static_cast<char>(kCommandFormatVersion));
  out.push_back(static_cast<char>(cmd.type));
  PutU32(out, static_cast<std::uint32_t>(cmd.key.size()));
  PutU32(out, static_cast<std::uint32_t>(cmd.value.size()));
  PutU32(out, static_cast<std::uint32_t>(cmd.request_id.size()));
  out += cmd.key;
  out += cmd.value;
  out += cmd.request_id;
  return out;
}

DecodeResult Decode(const std::string& bytes) {
  auto fail = [](std::string m) { return DecodeResult{std::nullopt, std::move(m)}; };
  if (bytes.size() < kCommandHeaderBytes) return fail("truncated header");
  const auto version = static_cast<std::uint8_t>(bytes[0]);
  if (version != kCommandFormatVersion) return fail("unsupported command format version " + std::to_string(version));
  const auto type = static_cast<std::uint8_t>(bytes[1]);
  if (type != static_cast<std::uint8_t>(CommandType::kPut) &&
      type != static_cast<std::uint8_t>(CommandType::kDelete))
    return fail("unknown command type " + std::to_string(type));
  const std::uint64_t klen = GetU32(bytes, 2), vlen = GetU32(bytes, 6), rlen = GetU32(bytes, 10);
  // 64-bit sum of three u32 cannot overflow, so this comparison is safe for any input.
  if (kCommandHeaderBytes + klen + vlen + rlen != bytes.size())
    return fail("length fields do not match the buffer size");
  Command c;
  c.type = static_cast<CommandType>(type);
  std::size_t pos = kCommandHeaderBytes;
  c.key.assign(bytes, pos, klen);
  pos += klen;
  c.value.assign(bytes, pos, vlen);
  pos += vlen;
  c.request_id.assign(bytes, pos, rlen);
  return DecodeResult{std::move(c), {}};
}

}  // namespace lsmdb::consensus_boundary
