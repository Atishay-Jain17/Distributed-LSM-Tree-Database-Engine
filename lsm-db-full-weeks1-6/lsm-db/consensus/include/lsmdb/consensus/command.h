#pragma once
#include <optional>
#include <string>

#include "lsmdb/v1/command.pb.h"

namespace lsmdb::consensus {

v1::ReplicatedCommand MakePutCommand(std::string key, std::string value);
v1::ReplicatedCommand MakeDeleteCommand(std::string key);

std::string EncodeCommand(const v1::ReplicatedCommand& command);

// nullopt if the bytes do not parse, the type is not PUT/DELETE, or the key is empty.
// Every replica makes the same decision for the same bytes, so skipping is deterministic.
std::optional<v1::ReplicatedCommand> DecodeCommand(const std::string& payload);

}  // namespace lsmdb::consensus
