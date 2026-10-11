#include "lsmdb/consensus/command.h"

namespace lsmdb::consensus {

v1::ReplicatedCommand MakePutCommand(std::string key, std::string value) {
  v1::ReplicatedCommand c;
  c.set_type(v1::COMMAND_TYPE_PUT);
  c.set_key(std::move(key));
  c.set_value(std::move(value));
  return c;
}

v1::ReplicatedCommand MakeDeleteCommand(std::string key) {
  v1::ReplicatedCommand c;
  c.set_type(v1::COMMAND_TYPE_DELETE);
  c.set_key(std::move(key));
  return c;
}

std::string EncodeCommand(const v1::ReplicatedCommand& command) {
  std::string out;
  command.SerializeToString(&out);
  return out;
}

std::optional<v1::ReplicatedCommand> DecodeCommand(const std::string& payload) {
  v1::ReplicatedCommand c;
  if (!c.ParseFromString(payload)) return std::nullopt;
  if (c.type() != v1::COMMAND_TYPE_PUT && c.type() != v1::COMMAND_TYPE_DELETE) return std::nullopt;
  if (c.key().empty()) return std::nullopt;
  return c;
}

}  // namespace lsmdb::consensus
