#pragma once
// Per-node network identity and peer settings used by the gRPC layer. The
// persistent data directory is passed separately to PersistentBackend.
#include <cstdint>
#include <string>
#include <vector>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::rpc {

// Parse a complete decimal uint32 value for command-line/environment options.
// Throws std::invalid_argument for empty, partial, signed, or out-of-range input.
std::uint32_t ParseUint32Argument(const std::string& text, const std::string& name);

struct NodeOptions {
  std::uint32_t node_id = 0;                // 1-based
  std::string advertise_host = "127.0.0.1"; // address other nodes use to reach this one
  std::uint32_t port = 0;                   // listen port; 0 = pick a free port (tests only)
  std::vector<v1::NodeInfo> peers;          // the OTHER nodes
};

// Parses "2=host2:50052,3=host3:50053". Empty string -> empty list.
// Throws std::invalid_argument on malformed input, bad ids/ports or duplicates.
std::vector<v1::NodeInfo> ParsePeers(const std::string& spec);

// Throws std::invalid_argument if options are unusable.
void ValidateNodeOptions(const NodeOptions& options, bool allow_ephemeral_port = false);

}  // namespace lsmdb::rpc
