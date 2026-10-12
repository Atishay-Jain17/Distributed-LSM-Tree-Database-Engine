#include "lsmdb/consensus_boundary/command.h"

#include <string>

#include "lsmdb/v1/command.pb.h"

namespace lsmdb::consensus_boundary {
namespace {

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
  v1::ReplicatedCommand proto;
  if (cmd.type == CommandType::kPut) {
    proto.set_type(v1::COMMAND_TYPE_PUT);
  } else if (cmd.type == CommandType::kDelete) {
    proto.set_type(v1::COMMAND_TYPE_DELETE);
  } else {
    proto.set_type(v1::COMMAND_TYPE_UNSPECIFIED);
  }
  proto.set_key(cmd.key);
  if (cmd.type == CommandType::kPut) {
    proto.set_value(cmd.value);
  }
  // TODO(vinayak): wire request_id once command.proto adds field 4 (Request #1 in docs/integration-requests.md)
  std::string out;
  proto.SerializeToString(&out);
  return out;
}

DecodeResult Decode(const std::string& bytes) {
  auto fail = [](std::string m) { return DecodeResult{std::nullopt, std::move(m)}; };
  if (bytes.empty()) return fail("empty command payload");

  v1::ReplicatedCommand proto;
  if (!proto.ParseFromString(bytes)) {
    return fail("failed to parse protobuf ReplicatedCommand");
  }

  // Reject unknown fields to maintain strictness
  if (proto.GetReflection()->GetUnknownFields(proto).field_count() > 0) {
    return fail("unknown protobuf fields rejected");
  }

  // Reject unspecified or unknown command types
  if (proto.type() != v1::COMMAND_TYPE_PUT && proto.type() != v1::COMMAND_TYPE_DELETE) {
    return fail("unknown or unspecified command type");
  }

  Command cmd;
  cmd.type = (proto.type() == v1::COMMAND_TYPE_PUT) ? CommandType::kPut : CommandType::kDelete;
  cmd.key = proto.key();
  cmd.value = proto.value();
  // TODO(vinayak): restore request_id from proto once field 4 is added (Request #1 in docs/integration-requests.md)
  cmd.request_id = "";

  return DecodeResult{std::move(cmd), {}};
}

}  // namespace lsmdb::consensus_boundary
